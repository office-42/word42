/* w42-math.c - see w42-math.h
 *
 * Copyright (C) 2026 Andreas Røsdal
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * The setting follows The TeXbook's Appendix G where MathML leaves room,
 * with its parameters in ems of the size being set: a superscript is
 * raised at least 0.363 em in the line and 0.413 em on a line of its own,
 * a fraction's bar sits on the axis, a quarter em up, level with a minus
 * sign's middle.  The glyphs' heights are their ink's, as TeX's are, and
 * their widths their advances.  No font need know it is setting
 * mathematics: an OpenType math font does it best, and a plain serif face
 * does it well enough, with the delimiters and the radical sign drawn
 * rather than stretched when what they hold is taller than a glyph.
 */

#include "w42-math.h"

#include <math.h>
#include <string.h>
#include <glib/gi18n.h>
#include <pango/pangocairo.h>

#include "w42-image.h"

/* ---------------------------------------------------------------------- */
/* The tree                                                                */
/* ---------------------------------------------------------------------- */

W42MathNode *
w42_math_node_new (const char *name, const char *text)
{
  W42MathNode *node = g_new0 (W42MathNode, 1);

  node->name = g_intern_string (name);
  node->children = g_ptr_array_new_with_free_func ((GDestroyNotify) w42_math_node_free);
  node->text = g_strdup (text);
  return node;
}

void
w42_math_node_free (W42MathNode *node)
{
  if (node == NULL)
    return;
  g_strfreev (node->attrs);
  g_ptr_array_unref (node->children);
  g_free (node->text);
  g_free (node);
}

void
w42_math_node_add (W42MathNode *parent, W42MathNode *child)
{
  g_return_if_fail (parent != NULL && child != NULL);
  g_ptr_array_add (parent->children, child);
}

const char *
w42_math_node_attr (const W42MathNode *node, const char *name)
{
  if (node == NULL || node->attrs == NULL)
    return NULL;
  for (guint i = 0; node->attrs[i] != NULL && node->attrs[i + 1] != NULL; i += 2)
    if (g_str_equal (node->attrs[i], name))
      return node->attrs[i + 1];
  return NULL;
}

void
w42_math_node_set_attr (W42MathNode *node, const char *name, const char *value)
{
  guint n = node->attrs != NULL ? g_strv_length (node->attrs) : 0;

  g_return_if_fail (node != NULL && name != NULL);
  for (guint i = 0; i + 1 < n; i += 2)
    if (g_str_equal (node->attrs[i], name))
      {
        g_free (node->attrs[i + 1]);
        node->attrs[i + 1] = g_strdup (value != NULL ? value : "");
        return;
      }
  node->attrs = g_renew (char *, node->attrs, n + 3);
  node->attrs[n] = g_strdup (name);
  node->attrs[n + 1] = g_strdup (value != NULL ? value : "");
  node->attrs[n + 2] = NULL;
}

static gboolean
is_token (const char *name)
{
  return g_str_equal (name, "mi") || g_str_equal (name, "mn") || g_str_equal (name, "mo") ||
         g_str_equal (name, "mtext") || g_str_equal (name, "ms");
}

/* The entities MathML names that a document is likely to use; XML knows
 * only five, and GMarkup no more. */
static const struct { const char *name; const char *utf8; } ENTITIES[] = {
  { "alpha", "α" }, { "beta", "β" }, { "gamma", "γ" }, { "delta", "δ" },
  { "epsilon", "ϵ" }, { "epsi", "ϵ" }, { "epsiv", "ε" }, { "varepsilon", "ε" },
  { "zeta", "ζ" }, { "eta", "η" }, { "theta", "θ" }, { "thetav", "ϑ" },
  { "vartheta", "ϑ" }, { "iota", "ι" }, { "kappa", "κ" }, { "lambda", "λ" },
  { "mu", "μ" }, { "nu", "ν" }, { "xi", "ξ" }, { "omicron", "ο" }, { "pi", "π" },
  { "piv", "ϖ" }, { "rho", "ρ" }, { "rhov", "ϱ" }, { "sigma", "σ" },
  { "sigmav", "ς" }, { "tau", "τ" }, { "upsilon", "υ" }, { "upsi", "υ" },
  { "phi", "ϕ" }, { "phiv", "φ" }, { "varphi", "φ" }, { "chi", "χ" },
  { "psi", "ψ" }, { "omega", "ω" },
  { "Alpha", "Α" }, { "Beta", "Β" }, { "Gamma", "Γ" }, { "Delta", "Δ" },
  { "Epsilon", "Ε" }, { "Zeta", "Ζ" }, { "Eta", "Η" }, { "Theta", "Θ" },
  { "Iota", "Ι" }, { "Kappa", "Κ" }, { "Lambda", "Λ" }, { "Mu", "Μ" },
  { "Nu", "Ν" }, { "Xi", "Ξ" }, { "Omicron", "Ο" }, { "Pi", "Π" },
  { "Rho", "Ρ" }, { "Sigma", "Σ" }, { "Tau", "Τ" }, { "Upsilon", "Υ" },
  { "Upsi", "ϒ" }, { "Phi", "Φ" }, { "Chi", "Χ" }, { "Psi", "Ψ" },
  { "Omega", "Ω" }, { "ohm", "Ω" },
  { "InvisibleTimes", "\342\201\242" }, { "it", "\342\201\242" },
  { "ApplyFunction", "\342\201\241" }, { "af", "\342\201\241" },
  { "InvisibleComma", "\342\201\243" }, { "ic", "\342\201\243" },
  { "nbsp", "\302\240" }, { "NonBreakingSpace", "\302\240" },
  { "ThinSpace", "\342\200\211" }, { "thinsp", "\342\200\211" },
  { "MediumSpace", "\342\201\237" }, { "ThickSpace", "\342\200\205" },
  { "VeryThinSpace", "\342\200\212" }, { "hairsp", "\342\200\212" },
  { "ensp", "\342\200\202" }, { "emsp", "\342\200\203" },
  { "NegativeThinSpace", "\342\200\213" }, { "ZeroWidthSpace", "\342\200\213" },
  { "minus", "−" }, { "plus", "+" }, { "pm", "±" }, { "plusmn", "±" },
  { "PlusMinus", "±" }, { "mp", "∓" }, { "mnplus", "∓" }, { "MinusPlus", "∓" },
  { "times", "×" }, { "divide", "÷" }, { "div", "÷" }, { "middot", "·" },
  { "centerdot", "·" }, { "CenterDot", "·" }, { "sdot", "⋅" }, { "cdot", "⋅" },
  { "compfn", "∘" }, { "SmallCircle", "∘" }, { "lowast", "∗" }, { "ast", "*" },
  { "star", "☆" }, { "sstarf", "⋆" }, { "Star", "⋆" }, { "setminus", "∖" },
  { "Backslash", "∖" }, { "bsol", "\\" },
  { "le", "≤" }, { "leq", "≤" }, { "LessEqual", "≤" }, { "ge", "≥" },
  { "geq", "≥" }, { "GreaterEqual", "≥" }, { "ne", "≠" }, { "NotEqual", "≠" },
  { "equiv", "≡" }, { "Congruent", "≡" }, { "approx", "≈" }, { "ap", "≈" },
  { "TildeTilde", "≈" }, { "sim", "∼" }, { "Tilde", "∼" }, { "simeq", "≃" },
  { "sime", "≃" }, { "cong", "≅" }, { "TildeFullEqual", "≅" }, { "prop", "∝" },
  { "propto", "∝" }, { "Proportional", "∝" }, { "ll", "≪" }, { "Lt", "≪" },
  { "gg", "≫" }, { "Gt", "≫" }, { "equals", "=" },
  { "colone", "≔" }, { "Assign", "≔" }, { "coloneq", "≔" },
  { "isin", "∈" }, { "isinv", "∈" }, { "in", "∈" }, { "Element", "∈" },
  { "notin", "∉" }, { "NotElement", "∉" }, { "ni", "∋" }, { "niv", "∋" },
  { "ReverseElement", "∋" }, { "sub", "⊂" }, { "subset", "⊂" }, { "sup", "⊃" },
  { "supset", "⊃" }, { "sube", "⊆" }, { "subseteq", "⊆" }, { "supe", "⊇" },
  { "supseteq", "⊇" }, { "nsub", "⊄" }, { "cap", "∩" }, { "cup", "∪" },
  { "Intersection", "⋂" }, { "Union", "⋃" }, { "xcap", "⋂" }, { "xcup", "⋃" },
  { "and", "∧" }, { "wedge", "∧" }, { "or", "∨" }, { "vee", "∨" }, { "not", "¬" },
  { "forall", "∀" }, { "ForAll", "∀" }, { "exist", "∃" }, { "Exists", "∃" },
  { "nexist", "∄" }, { "empty", "∅" }, { "emptyset", "∅" }, { "emptyv", "∅" },
  { "varnothing", "∅" }, { "infin", "∞" }, { "infty", "∞" }, { "part", "∂" },
  { "PartialD", "∂" }, { "nabla", "∇" }, { "Del", "∇" }, { "prime", "′" },
  { "Prime", "″" }, { "tprime", "‴" }, { "deg", "°" }, { "micro", "µ" },
  { "hbar", "ℏ" }, { "planck", "ℏ" }, { "ell", "ℓ" }, { "aleph", "ℵ" },
  { "weierp", "℘" }, { "wp", "℘" }, { "real", "ℜ" }, { "Re", "ℜ" },
  { "image", "ℑ" }, { "Im", "ℑ" }, { "imath", "ı" }, { "jmath", "ȷ" },
  { "angle", "∠" }, { "ang", "∠" }, { "perp", "⊥" }, { "bottom", "⊥" },
  { "UpTee", "⊥" }, { "top", "⊤" }, { "par", "∥" }, { "parallel", "∥" },
  { "DoubleVerticalBar", "∥" }, { "mid", "∣" }, { "VerticalBar", "∣" },
  { "verbar", "|" }, { "vert", "|" }, { "Verbar", "‖" }, { "Vert", "‖" },
  { "oplus", "⊕" }, { "CirclePlus", "⊕" }, { "otimes", "⊗" },
  { "CircleTimes", "⊗" }, { "ominus", "⊖" }, { "odot", "⊙" }, { "dagger", "†" },
  { "Dagger", "‡" }, { "bull", "•" }, { "bullet", "•" },
  { "sum", "∑" }, { "Sum", "∑" }, { "prod", "∏" }, { "Product", "∏" },
  { "coprod", "∐" }, { "Coproduct", "∐" }, { "int", "∫" }, { "Integral", "∫" },
  { "Int", "∬" }, { "iiint", "∭" }, { "tint", "∭" }, { "conint", "∮" },
  { "oint", "∮" }, { "ContourIntegral", "∮" }, { "radic", "√" }, { "Sqrt", "√" },
  { "rarr", "→" }, { "rightarrow", "→" }, { "RightArrow", "→" }, { "srarr", "→" },
  { "to", "→" }, { "larr", "←" }, { "leftarrow", "←" }, { "LeftArrow", "←" },
  { "harr", "↔" }, { "leftrightarrow", "↔" }, { "LeftRightArrow", "↔" },
  { "uarr", "↑" }, { "darr", "↓" }, { "rArr", "⇒" }, { "Rightarrow", "⇒" },
  { "Implies", "⇒" }, { "DoubleRightArrow", "⇒" }, { "lArr", "⇐" },
  { "Leftarrow", "⇐" }, { "DoubleLeftArrow", "⇐" }, { "hArr", "⇔" },
  { "iff", "⇔" }, { "Leftrightarrow", "⇔" }, { "DoubleLeftRightArrow", "⇔" },
  { "map", "↦" }, { "mapsto", "↦" }, { "RightTeeArrow", "↦" },
  { "longrightarrow", "⟶" }, { "LongRightArrow", "⟶" }, { "xrarr", "⟶" },
  { "lang", "⟨" }, { "langle", "⟨" }, { "LeftAngleBracket", "⟨" },
  { "rang", "⟩" }, { "rangle", "⟩" }, { "RightAngleBracket", "⟩" },
  { "lceil", "⌈" }, { "LeftCeiling", "⌈" }, { "rceil", "⌉" },
  { "RightCeiling", "⌉" }, { "lfloor", "⌊" }, { "LeftFloor", "⌊" },
  { "rfloor", "⌋" }, { "RightFloor", "⌋" }, { "lcub", "{" }, { "lbrace", "{" },
  { "rcub", "}" }, { "rbrace", "}" }, { "lpar", "(" }, { "rpar", ")" },
  { "lsqb", "[" }, { "lbrack", "[" }, { "rsqb", "]" }, { "rbrack", "]" },
  { "hellip", "…" }, { "mldr", "…" }, { "ctdot", "⋯" }, { "cdots", "⋯" },
  { "vellip", "⋮" }, { "vdots", "⋮" }, { "dtdot", "⋱" }, { "ddots", "⋱" },
  { "circ", "ˆ" }, { "Hat", "^" }, { "tilde", "˜" }, { "DiacriticalTilde", "˜" },
  { "macr", "¯" }, { "OverBar", "¯" }, { "strns", "¯" }, { "UnderBar", "_" },
  { "lowbar", "_" }, { "dot", "˙" }, { "DiacriticalDot", "˙" }, { "die", "¨" },
  { "uml", "¨" }, { "DoubleDot", "¨" }, { "acute", "´" }, { "grave", "`" },
  { "breve", "˘" }, { "caron", "ˇ" }, { "Hacek", "ˇ" }, { "OverBrace", "⏞" },
  { "UnderBrace", "⏟" }, { "OverParenthesis", "⏜" }, { "UnderParenthesis", "⏝" },
  { "Copf", "ℂ" }, { "complexes", "ℂ" }, { "Nopf", "ℕ" }, { "naturals", "ℕ" },
  { "Popf", "ℙ" }, { "primes", "ℙ" }, { "Qopf", "ℚ" }, { "rationals", "ℚ" },
  { "Ropf", "ℝ" }, { "reals", "ℝ" }, { "Zopf", "ℤ" }, { "integers", "ℤ" },
  { "therefore", "∴" }, { "there4", "∴" }, { "because", "∵" }, { "becaus", "∵" },
  { "comma", "," }, { "period", "." }, { "colon", ":" }, { "semi", ";" },
  { "excl", "!" }, { "quest", "?" }, { "num", "#" }, { "percnt", "%" },
  { "dollar", "$" }, { "lsquo", "‘" }, { "rsquo", "’" }, { "ldquo", "“" },
  { "rdquo", "”" }, { "ndash", "–" }, { "mdash", "—" }, { "copy", "©" },
  { "sect", "§" }, { "para", "¶" }, { "frac12", "½" }, { "half", "½" },
};

/* The text with MathML's named entities made characters, and a name
 * that is not one left as its own text rather than stopping the read. */
static char *
expand_entities (const char *s, gsize len)
{
  GString *out = g_string_sized_new (len + 16);

  for (gsize i = 0; i < len; i++)
    {
      if (s[i] == '&' && i + 1 < len && s[i + 1] != '#')
        {
          gsize j = i + 1;

          while (j < len && j - i < 40 && g_ascii_isalnum (s[j]))
            j++;
          if (j < len && s[j] == ';' && j > i + 1)
            {
              char name[48];
              const char *utf8 = NULL;

              memcpy (name, s + i + 1, j - i - 1);
              name[j - i - 1] = '\0';
              if (g_str_equal (name, "lt") || g_str_equal (name, "gt") ||
                  g_str_equal (name, "amp") || g_str_equal (name, "quot") ||
                  g_str_equal (name, "apos"))
                {
                  g_string_append_len (out, s + i, (gssize) (j - i + 1));
                  i = j;
                  continue;
                }
              for (guint k = 0; k < G_N_ELEMENTS (ENTITIES) && utf8 == NULL; k++)
                if (g_str_equal (ENTITIES[k].name, name))
                  utf8 = ENTITIES[k].utf8;
              if (utf8 != NULL)
                {
                  /* In an attribute a quote would end it: those five are
                   * written as XML's. */
                  if (g_str_equal (utf8, "<")) utf8 = "&lt;";
                  else if (g_str_equal (utf8, ">")) utf8 = "&gt;";
                  g_string_append (out, utf8);
                }
              else
                g_string_append_printf (out, "&amp;%s;", name);
              i = j;
              continue;
            }
          g_string_append (out, "&amp;");
          continue;
        }
      g_string_append_c (out, s[i]);
    }
  return g_string_free (out, FALSE);
}

typedef struct {
  W42MathNode *root;
  GPtrArray   *stack;      /* W42MathNode*, not owned */
  GPtrArray   *texts;      /* GString*, one per open element */
  int          skip;       /* depth inside elements outside the math */
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
  const char *name = local_name (element);
  W42MathNode *node;

  (void) ctx; (void) error;
  if (r->stack->len == 0 && r->root != NULL)
    {
      r->skip++;                  /* a second formula: not ours */
      return;
    }
  if (r->skip > 0)
    {
      r->skip++;
      return;
    }
  node = w42_math_node_new (name, NULL);
  for (int i = 0; names[i] != NULL; i++)
    {
      if (g_str_has_prefix (names[i], "xmlns"))
        continue;
      w42_math_node_set_attr (node, local_name (names[i]), values[i]);
    }
  if (r->stack->len == 0)
    r->root = node;
  else
    w42_math_node_add (g_ptr_array_index (r->stack, r->stack->len - 1), node);
  g_ptr_array_add (r->stack, node);
  g_ptr_array_add (r->texts, g_string_new (NULL));
}

/* A token's text as MathML reads it: the whitespace at its ends dropped
 * and each run of it within made one space. */
static char *
fold_space (const char *s)
{
  GString *out = g_string_new (NULL);
  gboolean space = FALSE;

  for (const char *p = s; *p != '\0'; p = g_utf8_next_char (p))
    {
      gunichar c = g_utf8_get_char (p);

      if (c == ' ' || c == '\t' || c == '\n' || c == '\r')
        {
          space = out->len > 0;
          continue;
        }
      if (space)
        g_string_append_c (out, ' ');
      space = FALSE;
      g_string_append_unichar (out, c);
    }
  return g_string_free (out, FALSE);
}

static void
on_end (GMarkupParseContext *ctx, const char *element, gpointer data, GError **error)
{
  Reader *r = data;
  W42MathNode *node;
  GString *text;

  (void) ctx; (void) element; (void) error;
  if (r->skip > 0)
    {
      r->skip--;
      return;
    }
  if (r->stack->len == 0)
    return;
  node = g_ptr_array_steal_index (r->stack, r->stack->len - 1);
  text = g_ptr_array_steal_index (r->texts, r->texts->len - 1);
  if (is_token (node->name))
    node->text = fold_space (text->str);
  else if (g_str_equal (node->name, "annotation"))
    node->text = g_strdup (text->str);
  g_string_free (text, TRUE);
}

static void
on_text (GMarkupParseContext *ctx, const char *text, gsize len, gpointer data, GError **error)
{
  Reader *r = data;

  (void) ctx; (void) error;
  if (r->skip == 0 && r->texts->len > 0)
    g_string_append_len (g_ptr_array_index (r->texts, r->texts->len - 1), text, (gssize) len);
}

W42MathNode *
w42_math_parse (const char *mathml, gssize len, GError **error)
{
  static const GMarkupParser PARSER = { on_start, on_end, on_text, NULL, NULL };
  Reader r = { NULL, g_ptr_array_new (), g_ptr_array_new (), 0 };
  GMarkupParseContext *ctx;
  char *xml;
  gboolean ok;

  g_return_val_if_fail (mathml != NULL, NULL);
  if (len < 0)
    len = (gssize) strlen (mathml);

  xml = expand_entities (mathml, (gsize) len);
  ctx = g_markup_parse_context_new (&PARSER, G_MARKUP_TREAT_CDATA_AS_TEXT, &r, NULL);
  ok = g_markup_parse_context_parse (ctx, xml, -1, error) &&
       g_markup_parse_context_end_parse (ctx, error);
  g_markup_parse_context_free (ctx);
  g_free (xml);
  for (guint i = 0; i < r.texts->len; i++)
    g_string_free (g_ptr_array_index (r.texts, i), TRUE);
  g_ptr_array_free (r.texts, TRUE);
  g_ptr_array_free (r.stack, TRUE);

  if (!ok)
    {
      w42_math_node_free (r.root);
      return NULL;
    }
  if (r.root == NULL)
    {
      g_set_error_literal (error, G_MARKUP_ERROR, G_MARKUP_ERROR_EMPTY, _("There is no MathML here."));
      return NULL;
    }
  /* A fragment -- an <mrow> on its own -- is a formula all the same. */
  if (!g_str_equal (r.root->name, "math"))
    {
      W42MathNode *math = w42_math_node_new ("math", NULL);

      w42_math_node_add (math, r.root);
      r.root = math;
    }
  return r.root;
}

static void
append_xml_text (GString *out, const char *s)
{
  for (const char *p = s; *p != '\0'; p++)
    switch (*p)
      {
      case '<':  g_string_append (out, "&lt;"); break;
      case '>':  g_string_append (out, "&gt;"); break;
      case '&':  g_string_append (out, "&amp;"); break;
      case '"':  g_string_append (out, "&quot;"); break;
      default:   g_string_append_c (out, *p);
      }
}

static void
serialize (GString *out, const W42MathNode *node, gboolean root)
{
  g_string_append_printf (out, "<%s", node->name);
  if (root)
    g_string_append (out, " xmlns=\"" W42_MATHML_NS "\"");
  for (guint i = 0; node->attrs != NULL && node->attrs[i] != NULL && node->attrs[i + 1] != NULL; i += 2)
    {
      g_string_append_printf (out, " %s=\"", node->attrs[i]);
      append_xml_text (out, node->attrs[i + 1]);
      g_string_append_c (out, '"');
    }
  if (node->children->len == 0 && (node->text == NULL || *node->text == '\0'))
    {
      g_string_append (out, "/>");
      return;
    }
  g_string_append_c (out, '>');
  if (node->text != NULL)
    append_xml_text (out, node->text);
  for (guint i = 0; i < node->children->len; i++)
    serialize (out, g_ptr_array_index (node->children, i), FALSE);
  g_string_append_printf (out, "</%s>", node->name);
}

char *
w42_math_node_to_string (const W42MathNode *node)
{
  GString *out = g_string_new (NULL);

  g_return_val_if_fail (node != NULL, NULL);
  serialize (out, node, TRUE);
  return g_string_free (out, FALSE);
}

gboolean
w42_math_is_mathml (const char *xml, gsize len)
{
  const char *p = xml, *end = xml + len;

  if (xml == NULL)
    return FALSE;
  /* Past the declaration, comments and a doctype, to the first element. */
  while (p < end)
    {
      const char *lt = memchr (p, '<', (gsize) (end - p));
      const char *name, *stop;

      if (lt == NULL || lt + 1 >= end)
        return FALSE;
      if (lt[1] == '?' || lt[1] == '!')
        {
          const char *gt = memchr (lt, '>', (gsize) (end - lt));

          if (gt == NULL)
            return FALSE;
          p = gt + 1;
          continue;
        }
      name = lt + 1;
      stop = name;
      while (stop < end && !g_ascii_isspace (*stop) && *stop != '>' && *stop != '/')
        stop++;
      for (const char *q = name; q < stop; q++)
        if (*q == ':')
          name = q + 1;
      return stop - name == 4 && strncmp (name, "math", 4) == 0;
    }
  return FALSE;
}

char *
w42_math_canonical (const char *mathml, gssize len)
{
  W42MathNode *root = mathml != NULL ? w42_math_parse (mathml, len, NULL) : NULL;
  char *out;

  if (root == NULL)
    return NULL;
  out = w42_math_node_to_string (root);
  w42_math_node_free (root);
  return out;
}

const char *
w42_math_annotation (const W42MathNode *root, const char *encoding)
{
  if (root == NULL)
    return NULL;
  if (g_str_equal (root->name, "annotation"))
    {
      const char *enc = w42_math_node_attr (root, "encoding");

      return enc != NULL && g_ascii_strcasecmp (enc, encoding) == 0 ? root->text : NULL;
    }
  if (!g_str_equal (root->name, "math") && !g_str_equal (root->name, "semantics") &&
      !g_str_equal (root->name, "mrow"))
    return NULL;
  for (guint i = 0; i < root->children->len; i++)
    {
      const char *found = w42_math_annotation (g_ptr_array_index (root->children, i), encoding);

      if (found != NULL)
        return found;
    }
  return NULL;
}

gboolean
w42_math_is_display (const W42MathNode *root)
{
  const char *display = w42_math_node_attr (root, "display");
  const char *mode = w42_math_node_attr (root, "mode");

  return (display != NULL && g_str_equal (display, "block")) ||
         (mode != NULL && g_str_equal (mode, "display"));
}

/* ---------------------------------------------------------------------- */
/* Boxes                                                                   */
/* ---------------------------------------------------------------------- */

/* The faces tried, in order: a mathematics font where there is one, then
 * the serif faces most systems have.  Pango falls back character by
 * character along the list. */
#define MATH_FAMILY "Cambria Math,STIX Two Math,Latin Modern Math,STIX Two Text,Times New Roman,Liberation Serif,DejaVu Serif,serif"
#define SANS_FAMILY "Liberation Sans,Arial,DejaVu Sans,sans-serif"
#define MONO_FAMILY "Liberation Mono,Courier New,DejaVu Sans Mono,monospace"

typedef enum {
  DRAW_LINE,          /* a stroke from (x0, y0) to (x1, y1) */
  DRAW_RECT,          /* a filled rectangle */
  DRAW_DELIM,         /* a delimiter grown to (x0, y0)-(x1, y1) */
  DRAW_HDELIM,        /* one lying down, over or under what it spans */
  DRAW_RADICAL,       /* a radical sign whose bar ends at x1 */
  DRAW_ELLIPSE
} DrawKind;

typedef struct {
  DrawKind  kind;
  gunichar  c;
  double    x0, y0, x1, y1;
  double    width;    /* the line's */
  double    extra;    /* DRAW_RADICAL: where the sign meets the bar */
} Draw;

typedef enum {
  OP_STRETCHY = 1 << 0,
  OP_LARGE    = 1 << 1,
  OP_MOVABLE  = 1 << 2,
  OP_FENCE    = 1 << 3,
  OP_ACCENT   = 1 << 4,
  OP_INTEGRAL = 1 << 5,
  OP_SEPARATOR = 1 << 6
} OpFlags;

struct _W42MathBox {
  double       w, asc, desc;
  PangoLayout *layout;       /* a token's glyphs */
  double       lx, ly;       /* where the layout's top left goes */
  double       italic;       /* how far the ink leans out past the advance */
  double       ink_low;      /* a glyph's ink's foot below the baseline:
                              * less than 0 for an accent, which is all
                              * above it */
  double       size;         /* the size the formula was set at */
  GPtrArray   *kids;         /* W42MathBox* */
  GArray      *at;           /* double x, y per kid: its origin, y down */
  GArray      *draws;        /* Draw */
  gboolean     has_color;
  guint32      color;
  gboolean     hidden;       /* mphantom: room, and nothing in it */
  /* For the row it is in: an operator's text and what it is. */
  char        *op;
  guint        op_flags;
  double       lspace, rspace;   /* set explicitly, in points, or -1 */
  double       minsize;      /* the least a stretchy one grows to */
  gboolean     token;        /* a single token: scripts hang on its ink */
};

typedef struct {
  PangoContext *ctx;
  double        axis;        /* the axis and the x-height, in ems */
  double        xheight;
} Env;

typedef struct {
  double       size;         /* the size being set, in points */
  double       base;         /* scriptlevel 0's */
  int          level;        /* scriptlevel */
  gboolean     display;      /* displaystyle */
  const char  *variant;      /* mathvariant, inherited through mstyle */
  gboolean     has_color;
  guint32      color;
} St;

static W42MathBox *
box_new (void)
{
  W42MathBox *b = g_new0 (W42MathBox, 1);

  b->kids = g_ptr_array_new_with_free_func ((GDestroyNotify) w42_math_box_free);
  b->at = g_array_new (FALSE, FALSE, sizeof (double));
  b->draws = g_array_new (FALSE, FALSE, sizeof (Draw));
  b->lspace = b->rspace = -1.0;
  return b;
}

void
w42_math_box_free (W42MathBox *box)
{
  if (box == NULL)
    return;
  g_clear_object (&box->layout);
  g_ptr_array_unref (box->kids);
  g_array_free (box->at, TRUE);
  g_array_free (box->draws, TRUE);
  g_free (box->op);
  g_free (box);
}

static void
box_put (W42MathBox *b, W42MathBox *kid, double x, double y)
{
  g_ptr_array_add (b->kids, kid);
  g_array_append_val (b->at, x);
  g_array_append_val (b->at, y);
}

static void
box_draw (W42MathBox *b, DrawKind kind, double x0, double y0, double x1, double y1,
          double width)
{
  Draw d = { kind, 0, x0, y0, x1, y1, width, 0.0 };

  g_array_append_val (b->draws, d);
}

/* Takes the height and depth from what is in the box, and the width
 * from its right edge. */
static void
box_fit (W42MathBox *b)
{
  b->asc = b->desc = 0.0;
  for (guint i = 0; i < b->kids->len; i++)
    {
      W42MathBox *k = g_ptr_array_index (b->kids, i);
      double y = g_array_index (b->at, double, 2 * i + 1);

      b->asc = MAX (b->asc, k->asc - y);
      b->desc = MAX (b->desc, k->desc + y);
    }
}

static double
st_em (const St *st)
{
  return st->size;
}

static St
st_script (const St *st, int levels)
{
  St s = *st;
  double f;

  s.level += levels;
  s.display = FALSE;
  /* TeX's script and scriptscript sizes, 70 and 50 per cent, and no
   * smaller however deep. */
  f = s.level <= 0 ? 1.0 : s.level == 1 ? 0.71 : 0.5;
  s.size = MAX (s.base * f, 1.0);
  return s;
}

/* ---- Lengths and colours --------------------------------------------- */

/* A MathML length in points, in a formula set at `em` points; `dflt` for
 * what is not one.  A percentage is of `pct_of`. */
static double
parse_length (const char *s, double em, double pct_of, double dflt)
{
  static const struct { const char *name; double ems; } NAMED[] = {
    { "veryverythinmathspace", 1 / 18.0 }, { "verythinmathspace", 2 / 18.0 },
    { "thinmathspace", 3 / 18.0 }, { "mediummathspace", 4 / 18.0 },
    { "thickmathspace", 5 / 18.0 }, { "verythickmathspace", 6 / 18.0 },
    { "veryverythickmathspace", 7 / 18.0 },
  };
  char *end;
  double v;
  gboolean negative = FALSE;

  if (s == NULL)
    return dflt;
  while (*s == ' ')
    s++;
  if (g_str_has_prefix (s, "negative"))
    {
      negative = TRUE;
      s += 8;
    }
  for (guint i = 0; i < G_N_ELEMENTS (NAMED); i++)
    if (g_ascii_strcasecmp (s, NAMED[i].name) == 0)
      return (negative ? -1 : 1) * NAMED[i].ems * em;
  v = g_ascii_strtod (s, &end);
  if (end == s)
    return dflt;
  while (*end == ' ')
    end++;
  if (*end == '\0')                   return v * em;   /* a bare number: ems */
  if (g_str_has_prefix (end, "em"))   return v * em;
  if (g_str_has_prefix (end, "ex"))   return v * em * 0.45;
  if (g_str_has_prefix (end, "pt"))   return v;
  if (g_str_has_prefix (end, "px"))   return v * 0.75;
  if (g_str_has_prefix (end, "in"))   return v * 72.0;
  if (g_str_has_prefix (end, "cm"))   return v * 72.0 / 2.54;
  if (g_str_has_prefix (end, "mm"))   return v * 72.0 / 25.4;
  if (g_str_has_prefix (end, "pc"))   return v * 12.0;
  if (*end == '%')                    return v / 100.0 * pct_of;
  return dflt;
}

static gboolean
parse_color (const char *s, guint32 *rgb)
{
  static const struct { const char *name; guint32 rgb; } NAMED[] = {
    { "black", 0x000000 }, { "white", 0xFFFFFF }, { "red", 0xFF0000 },
    { "green", 0x008000 }, { "blue", 0x0000FF }, { "yellow", 0xFFFF00 },
    { "gray", 0x808080 }, { "grey", 0x808080 }, { "silver", 0xC0C0C0 },
    { "maroon", 0x800000 }, { "purple", 0x800080 }, { "fuchsia", 0xFF00FF },
    { "magenta", 0xFF00FF }, { "lime", 0x00FF00 }, { "olive", 0x808000 },
    { "navy", 0x000080 }, { "teal", 0x008080 }, { "aqua", 0x00FFFF },
    { "cyan", 0x00FFFF }, { "orange", 0xFFA500 }, { "brown", 0xA52A2A },
  };

  if (s == NULL)
    return FALSE;
  if (s[0] == '#')
    {
      gsize n = strlen (s + 1);
      guint64 v;
      char *end;

      v = g_ascii_strtoull (s + 1, &end, 16);
      if (*end != '\0')
        return FALSE;
      if (n == 3)
        {
          *rgb = (guint32) (((v >> 8) & 0xF) * 0x110000 + ((v >> 4) & 0xF) * 0x1100 + (v & 0xF) * 0x11);
          return TRUE;
        }
      if (n == 6)
        {
          *rgb = (guint32) v;
          return TRUE;
        }
      return FALSE;
    }
  for (guint i = 0; i < G_N_ELEMENTS (NAMED); i++)
    if (g_ascii_strcasecmp (s, NAMED[i].name) == 0)
      {
        *rgb = NAMED[i].rgb;
        return TRUE;
      }
  return FALSE;
}

/* ---- Glyphs --------------------------------------------------------- */

/* A letter in one of MathML's alphabets: 𝔸 for A double-struck.  The
 * Letterlike Symbols block had some of them first, and Unicode left
 * holes in the alphabets where they were. */
static gunichar
variant_char (gunichar c, const char *variant)
{
  static const struct { const char *variant; gunichar upper, lower, digit; } ALPHA[] = {
    { "double-struck", 0x1D538, 0x1D552, 0x1D7D8 },
    { "script",        0x1D49C, 0x1D4B6, 0 },
    { "fraktur",       0x1D504, 0x1D51E, 0 },
    { "bold-script",   0x1D4D0, 0x1D4EA, 0 },
    { "bold-fraktur",  0x1D56C, 0x1D586, 0 },
  };
  static const struct { const char *variant; gunichar c, to; } HOLES[] = {
    { "double-struck", 'C', 0x2102 }, { "double-struck", 'H', 0x210D },
    { "double-struck", 'N', 0x2115 }, { "double-struck", 'P', 0x2119 },
    { "double-struck", 'Q', 0x211A }, { "double-struck", 'R', 0x211D },
    { "double-struck", 'Z', 0x2124 },
    { "script", 'B', 0x212C }, { "script", 'E', 0x2130 }, { "script", 'F', 0x2131 },
    { "script", 'H', 0x210B }, { "script", 'I', 0x2110 }, { "script", 'L', 0x2112 },
    { "script", 'M', 0x2133 }, { "script", 'R', 0x211B }, { "script", 'e', 0x212F },
    { "script", 'g', 0x210A }, { "script", 'o', 0x2134 },
    { "fraktur", 'C', 0x212D }, { "fraktur", 'H', 0x210C }, { "fraktur", 'I', 0x2111 },
    { "fraktur", 'R', 0x211C }, { "fraktur", 'Z', 0x2128 },
  };

  if (variant == NULL)
    return c;
  for (guint i = 0; i < G_N_ELEMENTS (HOLES); i++)
    if (HOLES[i].c == c && g_str_equal (HOLES[i].variant, variant))
      return HOLES[i].to;
  for (guint i = 0; i < G_N_ELEMENTS (ALPHA); i++)
    if (g_str_equal (ALPHA[i].variant, variant))
      {
        if (c >= 'A' && c <= 'Z')
          return ALPHA[i].upper + (c - 'A');
        if (c >= 'a' && c <= 'z')
          return ALPHA[i].lower + (c - 'a');
        if (c >= '0' && c <= '9' && ALPHA[i].digit != 0)
          return ALPHA[i].digit + (c - '0');
      }
  return c;
}

static W42MathBox *
text_box (Env *env, const St *st, const char *text, const char *family,
          gboolean bold, gboolean italic, double size)
{
  W42MathBox *b = box_new ();
  PangoFontDescription *desc = pango_font_description_new ();
  PangoRectangle ink, logical;
  double baseline;

  pango_font_description_set_family (desc, family);
  pango_font_description_set_weight (desc, bold ? PANGO_WEIGHT_BOLD : PANGO_WEIGHT_NORMAL);
  pango_font_description_set_style (desc, italic ? PANGO_STYLE_ITALIC : PANGO_STYLE_NORMAL);
  pango_font_description_set_absolute_size (desc, MAX (size, 0.5) * PANGO_SCALE);

  b->layout = pango_layout_new (env->ctx);
  pango_layout_set_font_description (b->layout, desc);
  pango_layout_set_text (b->layout, text, -1);
  pango_font_description_free (desc);

  pango_layout_get_extents (b->layout, &ink, &logical);
  baseline = (double) pango_layout_get_baseline (b->layout) / PANGO_SCALE;
  b->w = (double) logical.width / PANGO_SCALE;
  if (ink.width > 0 && ink.height > 0)
    {
      b->asc = MAX (baseline - (double) ink.y / PANGO_SCALE, 0.0);
      b->desc = MAX ((double) (ink.y + ink.height) / PANGO_SCALE - baseline, 0.0);
      b->italic = MAX ((double) (ink.x + ink.width - logical.x - logical.width) / PANGO_SCALE, 0.0);
      b->ink_low = (double) (ink.y + ink.height) / PANGO_SCALE - baseline;
    }
  b->lx = -(double) logical.x / PANGO_SCALE;
  b->ly = -baseline;
  b->has_color = st->has_color;
  b->color = st->color;
  b->token = TRUE;
  return b;
}

/* Scales a token's glyphs about its baseline. */
static void
box_scale_text (W42MathBox *b, double factor)
{
  PangoFontDescription *desc;

  if (b->layout == NULL || factor == 1.0)
    return;
  desc = pango_font_description_copy (pango_layout_get_font_description (b->layout));
  pango_font_description_set_absolute_size (desc,
    pango_font_description_get_size (desc) * factor);
  pango_layout_set_font_description (b->layout, desc);
  pango_font_description_free (desc);
  {
    PangoRectangle ink, logical;
    double baseline = (double) pango_layout_get_baseline (b->layout) / PANGO_SCALE;

    pango_layout_get_extents (b->layout, &ink, &logical);
    b->w = (double) logical.width / PANGO_SCALE;
    b->asc = MAX (baseline - (double) ink.y / PANGO_SCALE, 0.0);
    b->desc = MAX ((double) (ink.y + ink.height) / PANGO_SCALE - baseline, 0.0);
    b->italic = MAX ((double) (ink.x + ink.width - logical.x - logical.width) / PANGO_SCALE, 0.0);
    b->ink_low = (double) (ink.y + ink.height) / PANGO_SCALE - baseline;
    b->lx = -(double) logical.x / PANGO_SCALE;
    b->ly = -baseline;
  }
}

/* ---- Operators -------------------------------------------------------- */

typedef enum { FORM_PREFIX, FORM_INFIX, FORM_POSTFIX } Form;

static gboolean
in_set (gunichar c, const char *set)
{
  for (const char *p = set; *p != '\0'; p = g_utf8_next_char (p))
    if (g_utf8_get_char (p) == c)
      return TRUE;
  return FALSE;
}

#define FENCES     "()[]{}|‖⟨⟩⌈⌉⌊⌋⟦⟧⟮⟯"
#define RELATIONS  "=<>≤≥≠≈≡∼≃≅∝∈∉∋⊂⊃⊆⊇⊄⊥∥≪≫≺≻⪯⪰⊢⊨≔→←↔⇒⇐⇔↦⟶⟵⟷⟹⟸⟺↑↓≐≜∣≲≳⊲⊳"
#define BINARY     "+−-±∓×÷·⋅∘∗∪∩∧∨⊕⊗⊖⊙∖⋆†‡⊓⊔⊎"
#define LARGE      "∑∏∐⋃⋂⨁⨂⨀⋀⋁⨄"
#define INTEGRALS  "∫∬∭∮∯∰⨌"

/* What an operator is, and the space round it in ems, as MathML's
 * operator dictionary says for the ones mathematics uses most. */
static guint
op_lookup (const char *text, Form form, double *lspace, double *rspace)
{
  gunichar c;
  static const char *const MOVABLE_NAMES[] = {
    "lim", "max", "min", "sup", "inf", "liminf", "limsup", "det", "gcd", "Pr",
    "lim inf", "lim sup", NULL };

  *lspace = *rspace = 5 / 18.0;
  if (text == NULL || *text == '\0')
    {
      *lspace = *rspace = 0.0;
      return 0;
    }
  c = g_utf8_get_char (text);
  if (*g_utf8_next_char (text) != '\0')
    {
      /* A word: lim, max, or ":=". */
      for (guint i = 0; MOVABLE_NAMES[i] != NULL; i++)
        if (g_str_equal (text, MOVABLE_NAMES[i]))
          {
            *lspace = 1 / 18.0;
            *rspace = 3 / 18.0;
            return OP_MOVABLE | OP_LARGE;
          }
      if (g_str_equal (text, ":=") || g_str_equal (text, "==") || g_str_equal (text, "!="))
        return 0;
      *lspace = 0.0;
      *rspace = 3 / 18.0;
      return 0;
    }
  if (in_set (c, FENCES))
    {
      /* A bar between two things is a relation: { x | x > 0 }. */
      if ((c == '|' || c == 0x2016) && form == FORM_INFIX)
        return 0;
      *lspace = *rspace = 0.0;
      return OP_STRETCHY | OP_FENCE;
    }
  if (in_set (c, LARGE))
    {
      *lspace = 1 / 18.0;
      *rspace = 3 / 18.0;
      return OP_LARGE | OP_MOVABLE;
    }
  if (in_set (c, INTEGRALS))
    {
      *lspace = 0.0;
      *rspace = 2 / 18.0;
      return OP_LARGE | OP_INTEGRAL;
    }
  if (in_set (c, RELATIONS))
    {
      if (form != FORM_INFIX && c != 0x2192 && c != 0x2190)
        *lspace = *rspace = 0.0;
      return 0;
    }
  if (in_set (c, BINARY))
    {
      if (form == FORM_INFIX)
        *lspace = *rspace = 4 / 18.0;
      else
        *lspace = *rspace = 0.0;
      return 0;
    }
  switch (c)
    {
    case ',': case ';':
      *lspace = 0.0;
      *rspace = 3 / 18.0;
      return OP_SEPARATOR;
    case ':':
      *lspace = *rspace = form == FORM_INFIX ? 3 / 18.0 : 0.0;
      return 0;
    case 0x2061:                      /* function application */
    case 0x2062: case 0x2063: case 0x2064:
    case '.': case '!': case '\'': case 0x2032: case 0x2033: case 0x2034:
    case '%': case 0x2026: case 0x22EF: case 0x22EE: case 0x22F1: case '?':
    case 0x2202: case 0x2207: case 0x221E:
      *lspace = *rspace = 0.0;
      return 0;
    case '/':
      *lspace = *rspace = 1 / 18.0;
      return 0;
    case 0x2044: case 0x2215:
      *lspace = *rspace = 1 / 18.0;
      return 0;
    case 0xAF: case 0x203E: case '_': case '^': case '~': case 0x2C6: case 0x2DC:
    case 0x2D9: case 0xA8: case 0xB4: case '`': case 0x2D8: case 0x2C7: case 0x20D7:
    case 0x23DE: case 0x23DF: case 0x23DC: case 0x23DD:
      *lspace = *rspace = 0.0;
      return OP_ACCENT | OP_STRETCHY;
    default:
      if (form != FORM_INFIX)
        *lspace = *rspace = 0.0;
      return 0;
    }
}

static gboolean
attr_true (const W42MathNode *n, const char *name, gboolean dflt)
{
  const char *v = w42_math_node_attr (n, name);

  return v == NULL ? dflt : g_ascii_strcasecmp (v, "true") == 0;
}

/* ---- Drawn delimiters ------------------------------------------------- */

/* How wide a delimiter drawn `h` points tall is, at `em`. */
static double
delim_width (gunichar c, double h, double em)
{
  double grow = MIN (h * 0.04, 0.25 * em);

  switch (c)
    {
    case '(': case ')': case 0x27EE: case 0x27EF: return 0.42 * em + grow;
    case '[': case ']': case 0x27E6: case 0x27E7:  return 0.33 * em + grow / 2;
    case 0x2308: case 0x2309: case 0x230A: case 0x230B: return 0.36 * em;
    case '{': case '}':                          return 0.5 * em + grow;
    case '|':                                    return 0.28 * em;
    case 0x2016:                                 return 0.44 * em;
    case 0x27E8: case 0x27E9:                    return 0.4 * em + grow;
    default:                                     return 0.4 * em;
    }
}

static void
draw_delim (cairo_t *cr, const Draw *d)
{
  double x0 = d->x0, x1 = d->x1, top = d->y0, bottom = d->y1, t = d->width;
  double h = bottom - top, w = x1 - x0;
  double mid = (top + bottom) / 2.0;
  gboolean right = d->c == ')' || d->c == ']' || d->c == '}' || d->c == 0x27E9 ||
                   d->c == 0x2309 || d->c == 0x230B || d->c == 0x27E7 || d->c == 0x27EF;
  /* Drawn for the left one; mirrored for the right. */
  double pad = w * 0.18;

  cairo_save (cr);
  if (right)
    {
      cairo_translate (cr, x0 + x1, 0);
      cairo_scale (cr, -1, 1);
    }
  cairo_set_line_width (cr, t);
  cairo_set_line_cap (cr, CAIRO_LINE_CAP_BUTT);
  cairo_set_line_join (cr, CAIRO_LINE_JOIN_MITER);
  switch (d->c)
    {
    case '(': case ')': case 0x27EE: case 0x27EF:
      {
        /* A crescent, thick in the middle and fine at the ends. */
        double xr = x1 - pad, xl = x0 + pad * 0.6;
        double k = MIN (h * 0.3, h * 0.5);

        cairo_move_to (cr, xr, top);
        cairo_curve_to (cr, xl, top + k, xl, bottom - k, xr, bottom);
        cairo_curve_to (cr, xl + t * 1.8, bottom - k, xl + t * 1.8, top + k, xr, top);
        cairo_close_path (cr);
        cairo_fill (cr);
      }
      break;
    case '[': case ']':
      cairo_move_to (cr, x1 - pad, top + t / 2);
      cairo_line_to (cr, x0 + pad + t / 2, top + t / 2);
      cairo_line_to (cr, x0 + pad + t / 2, bottom - t / 2);
      cairo_line_to (cr, x1 - pad, bottom - t / 2);
      cairo_stroke (cr);
      break;
    case 0x27E6: case 0x27E7:
      cairo_move_to (cr, x1 - pad, top + t / 2);
      cairo_line_to (cr, x0 + pad + t / 2, top + t / 2);
      cairo_line_to (cr, x0 + pad + t / 2, bottom - t / 2);
      cairo_line_to (cr, x1 - pad, bottom - t / 2);
      cairo_move_to (cr, x0 + pad + t * 2.5, top + t);
      cairo_line_to (cr, x0 + pad + t * 2.5, bottom - t);
      cairo_stroke (cr);
      break;
    case 0x2308: case 0x2309:
      cairo_move_to (cr, x1 - pad, top + t / 2);
      cairo_line_to (cr, x0 + pad + t / 2, top + t / 2);
      cairo_line_to (cr, x0 + pad + t / 2, bottom);
      cairo_stroke (cr);
      break;
    case 0x230A: case 0x230B:
      cairo_move_to (cr, x0 + pad + t / 2, top);
      cairo_line_to (cr, x0 + pad + t / 2, bottom - t / 2);
      cairo_line_to (cr, x1 - pad, bottom - t / 2);
      cairo_stroke (cr);
      break;
    case '{': case '}':
      {
        double xl = x0 + pad, xr = x1 - pad, xm = (xl + xr) / 2.0;
        double r = MIN ((xr - xl) / 2.0, h / 8.0);

        cairo_set_line_width (cr, t * 0.9);
        cairo_move_to (cr, xr, top + t / 2);
        cairo_curve_to (cr, xm, top + t / 2, xm, top + t / 2, xm, top + r);
        cairo_line_to (cr, xm, mid - r);
        cairo_curve_to (cr, xm, mid, xm, mid, xl, mid);
        cairo_curve_to (cr, xm, mid, xm, mid, xm, mid + r);
        cairo_line_to (cr, xm, bottom - r);
        cairo_curve_to (cr, xm, bottom - t / 2, xm, bottom - t / 2, xr, bottom - t / 2);
        cairo_stroke (cr);
      }
      break;
    case '|':
      cairo_move_to (cr, (x0 + x1) / 2.0, top);
      cairo_line_to (cr, (x0 + x1) / 2.0, bottom);
      cairo_stroke (cr);
      break;
    case 0x2016:
      cairo_move_to (cr, x0 + w * 0.33, top);
      cairo_line_to (cr, x0 + w * 0.33, bottom);
      cairo_move_to (cr, x0 + w * 0.67, top);
      cairo_line_to (cr, x0 + w * 0.67, bottom);
      cairo_stroke (cr);
      break;
    case 0x27E8: case 0x27E9:
      cairo_move_to (cr, x1 - pad, top);
      cairo_line_to (cr, x0 + pad, mid);
      cairo_line_to (cr, x1 - pad, bottom);
      cairo_stroke (cr);
      break;
    default:
      break;
    }
  cairo_restore (cr);
}

/* Over or under what it spans: a bar, an arrow, a brace, a hat or a
 * tilde, from x0 to x1 in the band y0 to y1. */
static void
draw_hdelim (cairo_t *cr, const Draw *d)
{
  double x0 = d->x0, x1 = d->x1, y0 = d->y0, y1 = d->y1, t = d->width;
  double mid = (y0 + y1) / 2.0, h = y1 - y0, xm = (x0 + x1) / 2.0;

  cairo_save (cr);
  cairo_set_line_width (cr, t);
  cairo_set_line_cap (cr, CAIRO_LINE_CAP_BUTT);
  switch (d->c)
    {
    case 0xAF: case 0x203E: case '_': case 0x2015: case 0x2212:
      cairo_move_to (cr, x0, mid);
      cairo_line_to (cr, x1, mid);
      cairo_stroke (cr);
      break;
    case 0x2192: case 0x2190: case 0x2194: case 0x20D7: case 0x27F6: case 0x27F5:
      {
        double head = MIN (h * 0.9, (x1 - x0) / 3.0);
        gboolean r = d->c != 0x2190 && d->c != 0x27F5, l = d->c == 0x2190 || d->c == 0x2194 || d->c == 0x27F5;

        cairo_move_to (cr, x0, mid);
        cairo_line_to (cr, x1, mid);
        cairo_stroke (cr);
        cairo_set_line_join (cr, CAIRO_LINE_JOIN_ROUND);
        if (r)
          {
            cairo_move_to (cr, x1 - head, mid - head * 0.55);
            cairo_line_to (cr, x1, mid);
            cairo_line_to (cr, x1 - head, mid + head * 0.55);
            cairo_stroke (cr);
          }
        if (l)
          {
            cairo_move_to (cr, x0 + head, mid - head * 0.55);
            cairo_line_to (cr, x0, mid);
            cairo_line_to (cr, x0 + head, mid + head * 0.55);
            cairo_stroke (cr);
          }
      }
      break;
    case 0x23DE: case 0x23DF: case 0x23DC: case 0x23DD:
      {
        /* A brace lying down: its point away from what it spans. */
        gboolean over = d->c == 0x23DE || d->c == 0x23DC;
        double e = over ? y1 : y0, p = over ? y0 : y1, r = MIN (h / 2.0, (x1 - x0) / 8.0);
        double hm = (e + p) / 2.0;

        if (d->c == 0x23DC || d->c == 0x23DD)
          {
            cairo_move_to (cr, x0, e);
            cairo_curve_to (cr, x0 + (x1 - x0) * 0.1, p, x1 - (x1 - x0) * 0.1, p, x1, e);
            cairo_stroke (cr);
            break;
          }
        cairo_move_to (cr, x0, e);
        cairo_curve_to (cr, x0, hm, x0, hm, x0 + r, hm);
        cairo_line_to (cr, xm - r, hm);
        cairo_curve_to (cr, xm, hm, xm, hm, xm, p);
        cairo_curve_to (cr, xm, hm, xm, hm, xm + r, hm);
        cairo_line_to (cr, x1 - r, hm);
        cairo_curve_to (cr, x1, hm, x1, hm, x1, e);
        cairo_stroke (cr);
      }
      break;
    case '^': case 0x2C6: case 0x302:
      cairo_set_line_join (cr, CAIRO_LINE_JOIN_MITER);
      cairo_move_to (cr, x0, y1);
      cairo_line_to (cr, xm, y0);
      cairo_line_to (cr, x1, y1);
      cairo_stroke (cr);
      break;
    case 0x2C7:
      cairo_move_to (cr, x0, y0);
      cairo_line_to (cr, xm, y1);
      cairo_line_to (cr, x1, y0);
      cairo_stroke (cr);
      break;
    case '~': case 0x2DC: case 0x303: case 0x223C:
      cairo_move_to (cr, x0, y1 - h * 0.2);
      cairo_curve_to (cr, x0 + (x1 - x0) * 0.3, y0 - h * 0.2, x0 + (x1 - x0) * 0.7, y1 + h * 0.2,
                      x1, y0 + h * 0.2);
      cairo_stroke (cr);
      break;
    default:
      break;
    }
  cairo_restore (cr);
}

/* A character that can be drawn lying down across what it spans. */
static gboolean
hstretchy (gunichar c)
{
  return in_set (c, "¯‾_→←↔⃗⟶⟵⏞⏟⏜⏝^ˆ̂ˇ~˜̃") || c == 0x2212;
}

/* ---------------------------------------------------------------------- */
/* Laying out                                                              */
/* ---------------------------------------------------------------------- */

static W42MathBox *lay (Env *env, const W42MathNode *n, const St *st);
static W42MathBox *lay_row (Env *env, const W42MathNode *n, guint from, const St *st);

static const char *
node_text (const W42MathNode *n)
{
  return n != NULL && n->text != NULL ? n->text : "";
}

/* The mo at the heart of an embellished operator -- ∑ with its limits,
 * an mo in an mrow of one -- or NULL. */
static const W42MathNode *
core_operator (const W42MathNode *n)
{
  while (n != NULL)
    {
      if (g_str_equal (n->name, "mo"))
        return n;
      if ((g_str_equal (n->name, "msub") || g_str_equal (n->name, "msup") ||
           g_str_equal (n->name, "msubsup") || g_str_equal (n->name, "munder") ||
           g_str_equal (n->name, "mover") || g_str_equal (n->name, "munderover") ||
           g_str_equal (n->name, "mfrac") || g_str_equal (n->name, "semantics") ||
           g_str_equal (n->name, "mstyle") || g_str_equal (n->name, "mrow") ||
           g_str_equal (n->name, "mpadded")) &&
          n->children->len >= 1)
        {
          if ((g_str_equal (n->name, "mrow") || g_str_equal (n->name, "mstyle") ||
               g_str_equal (n->name, "mpadded")) && n->children->len != 1)
            return NULL;
          n = g_ptr_array_index (n->children, 0);
          continue;
        }
      return NULL;
    }
  return NULL;
}

/* The style an element sets for what is in it: mstyle's attributes, and
 * a token's mathvariant and mathcolor. */
static St
st_from (const W42MathNode *n, const St *st)
{
  St s = *st;
  const char *v;
  guint32 rgb;

  if ((v = w42_math_node_attr (n, "displaystyle")) != NULL)
    s.display = g_ascii_strcasecmp (v, "true") == 0;
  if ((v = w42_math_node_attr (n, "scriptlevel")) != NULL)
    {
      int level = (int) g_ascii_strtoll (v, NULL, 10);
      int now = (v[0] == '+' || v[0] == '-') ? s.level + level : level;
      gboolean display = s.display;

      s = st_script (&s, now - s.level);
      s.display = display;
    }
  if ((v = w42_math_node_attr (n, "mathvariant")) != NULL)
    s.variant = g_intern_string (v);
  if ((parse_color (w42_math_node_attr (n, "mathcolor"), &rgb) ||
       parse_color (w42_math_node_attr (n, "color"), &rgb)))
    {
      s.has_color = TRUE;
      s.color = rgb;
    }
  if ((v = w42_math_node_attr (n, "mathsize")) != NULL ||
      (v = w42_math_node_attr (n, "fontsize")) != NULL)
    {
      if (g_ascii_strcasecmp (v, "small") == 0)
        s.size *= 0.8;
      else if (g_ascii_strcasecmp (v, "big") == 0)
        s.size *= 1.2;
      else if (g_ascii_strcasecmp (v, "normal") != 0)
        s.size = MAX (parse_length (v, s.size, s.size, s.size), 1.0);
    }
  return s;
}

/* A token: mi, mn, mo, mtext or ms. */
static W42MathBox *
lay_token (Env *env, const W42MathNode *n, const St *st_in)
{
  St st = st_from (n, st_in);
  const char *text = node_text (n);
  const char *variant = st.variant;
  gboolean mi = g_str_equal (n->name, "mi");
  gboolean italic = FALSE, bold = FALSE;
  const char *family = MATH_FAMILY;
  GString *shown = g_string_new (NULL);
  W42MathBox *b;

  /* A single letter in an mi is a variable, in italics; a name of
   * several letters is upright, as are capital Greek letters, TeX's
   * way. */
  if (mi && variant == NULL && g_utf8_strlen (text, -1) == 1)
    {
      gunichar c = g_utf8_get_char (text);

      italic = g_unichar_isalpha (c) && !(c >= 0x391 && c <= 0x3A9) && c != 0x221E;
    }
  if (variant != NULL)
    {
      if (g_str_equal (variant, "bold")) bold = TRUE;
      else if (g_str_equal (variant, "italic")) italic = TRUE;
      else if (g_str_equal (variant, "bold-italic")) bold = italic = TRUE;
      else if (g_str_has_prefix (variant, "sans-serif"))
        {
          family = SANS_FAMILY;
          bold = strstr (variant, "bold") != NULL;
          italic = strstr (variant, "italic") != NULL;
        }
      else if (g_str_equal (variant, "monospace")) family = MONO_FAMILY;
    }

  if (g_str_equal (n->name, "ms"))
    {
      const char *lq = w42_math_node_attr (n, "lquote"), *rq = w42_math_node_attr (n, "rquote");

      g_string_append (shown, lq != NULL ? lq : "\"");
      g_string_append (shown, text);
      g_string_append (shown, rq != NULL ? rq : "\"");
    }
  else
    for (const char *p = text; *p != '\0'; p = g_utf8_next_char (p))
      {
        gunichar c = g_utf8_get_char (p);

        /* A hyphen in an operator is a minus sign, an apostrophe a
         * prime, and a caret or a tilde on its own an accent's. */
        if (g_str_equal (n->name, "mo") && c == '-')
          c = 0x2212;
        else if (g_str_equal (n->name, "mo") && c == '\'')
          c = 0x2032;
        else if (g_str_equal (n->name, "mo") && c == '^' && *g_utf8_next_char (text) == '\0')
          c = 0x2C6;
        else if (g_str_equal (n->name, "mo") && c == '~' && *g_utf8_next_char (text) == '\0')
          c = 0x2DC;
        /* Function application and invisible times take no room. */
        else if (c == 0x2061 || c == 0x2062 || c == 0x2063 || c == 0x2064)
          continue;
        g_string_append_unichar (shown, variant_char (c, variant));
      }

  b = text_box (env, &st, shown->str, family, bold, italic, st.size);
  g_string_free (shown, TRUE);

  if (g_str_equal (n->name, "mo"))
    {
      double l, r;
      const char *v;

      b->op = g_strdup (text);
      /* The prefix form's: a bar is a fence until a row finds it
       * between two things. */
      b->op_flags = op_lookup (text, FORM_PREFIX, &l, &r);
      if (!attr_true (n, "stretchy", (b->op_flags & OP_STRETCHY) != 0))
        b->op_flags &= ~OP_STRETCHY;
      else
        b->op_flags |= OP_STRETCHY;
      if (!attr_true (n, "largeop", (b->op_flags & OP_LARGE) != 0))
        b->op_flags &= ~OP_LARGE;
      if (!attr_true (n, "movablelimits", (b->op_flags & OP_MOVABLE) != 0))
        b->op_flags &= ~OP_MOVABLE;
      if (attr_true (n, "accent", (b->op_flags & OP_ACCENT) != 0))
        b->op_flags |= OP_ACCENT;
      else
        b->op_flags &= ~OP_ACCENT;
      if ((v = w42_math_node_attr (n, "lspace")) != NULL)
        b->lspace = parse_length (v, st.size, 0.0, 0.0);
      if ((v = w42_math_node_attr (n, "rspace")) != NULL)
        b->rspace = parse_length (v, st.size, 0.0, 0.0);
      /* \big and its kin: a delimiter made at least this tall. */
      if ((v = w42_math_node_attr (n, "minsize")) != NULL)
        b->minsize = parse_length (v, st.size, (b->asc + b->desc), 0.0);

      /* A large operator is larger on a line of its own, and centred on
       * the axis either way.  A word -- lim, max -- is only an operator. */
      if ((b->op_flags & OP_LARGE) && g_utf8_strlen (text, -1) == 1)
        {
          double want = (b->op_flags & OP_INTEGRAL) ? (st.display ? 2.2 : 1.3)
                                                    : (st.display ? 1.4 : 1.0);
          double have = b->asc + b->desc;

          if (have > 0.0 && have < want * st.size)
            box_scale_text (b, want * st.size / have);
          {
            double axis = env->axis * st.size;
            double shift = (b->asc - b->desc) / 2.0 - axis;

            b->ly += shift;
            b->asc -= shift;
            b->desc += shift;
          }
          b->token = FALSE;
        }
    }
  return b;
}

/* Grows a delimiter to cover `asc` over and `desc` under the baseline,
 * symmetrically about the axis, as TeX's \left and \right do. */
static void
stretch_delim (Env *env, W42MathBox *b, double asc, double desc, double em)
{
  double axis = env->axis * em;
  double half = MAX (asc - axis, desc + axis);
  double need = MAX (2.0 * half * 0.901, 2.0 * half - 0.5 * em);
  gunichar c;

  if (b->op == NULL || b->layout == NULL)
    return;
  c = g_utf8_get_char (b->op);
  if (need <= (b->asc + b->desc) * 1.05)
    return;
  if (!in_set (c, FENCES))
    {
      /* A character we have no drawing for: its glyph, taller. */
      box_scale_text (b, need / MAX (b->asc + b->desc, 0.1));
      return;
    }
  g_clear_object (&b->layout);
  b->w = delim_width (c, need, em);
  b->asc = axis + need / 2.0;
  b->desc = need / 2.0 - axis;
  {
    Draw d = { DRAW_DELIM, c, 0.0, -b->asc, b->w, b->desc, 0.07 * em, 0.0 };

    g_array_append_val (b->draws, d);
  }
}

static W42MathBox *
lay_row (Env *env, const W42MathNode *n, guint from, const St *st)
{
  W42MathBox *row = box_new ();
  GPtrArray *boxes = g_ptr_array_new ();
  double asc = 0.0, desc = 0.0, x = 0.0;
  gboolean any_plain = FALSE;
  guint count = n->children->len > from ? n->children->len - from : 0;

  for (guint i = from; i < n->children->len; i++)
    {
      const W42MathNode *kid = g_ptr_array_index (n->children, i);
      W42MathBox *b = lay (env, kid, st);

      g_ptr_array_add (boxes, b);
      if (!(b->op_flags & OP_STRETCHY) || b->layout == NULL)
        {
          asc = MAX (asc, b->asc);
          desc = MAX (desc, b->desc);
          any_plain = TRUE;
        }
    }

  for (guint i = 0; i < boxes->len; i++)
    {
      W42MathBox *b = g_ptr_array_index (boxes, i);
      double l = 0.0, r = 0.0;

      if (b->op != NULL)
        {
          Form form = count == 1 ? FORM_INFIX : i == 0 ? FORM_PREFIX
                    : i + 1 == boxes->len ? FORM_POSTFIX : FORM_INFIX;
          guint flags = op_lookup (b->op, form, &l, &r);

          l *= st_em (st);
          r *= st_em (st);
          /* In scripts, TeX leaves out the space round operators. */
          if (st->level > 0 && !(flags & OP_SEPARATOR))
            l = r = 0.0;
          if (b->lspace >= 0.0) l = b->lspace;
          if (b->rspace >= 0.0) r = b->rspace;
          /* A function's name has a thin space after it, unless a
           * bracket follows. */
          if (g_utf8_get_char (b->op) == 0x2061 && i + 1 < boxes->len)
            {
              W42MathBox *next = g_ptr_array_index (boxes, i + 1);

              r = next->op != NULL && (next->op_flags & OP_FENCE) ? 0.0 : st_em (st) / 6.0;
            }
          if ((b->op_flags & OP_STRETCHY) && (flags & OP_FENCE) && any_plain &&
              b->layout != NULL)
            stretch_delim (env, b, asc, desc, st_em (st));
          /* The bar in { x | x > 0 } takes the height of what is round it. */
          if ((form == FORM_INFIX) && (flags & OP_FENCE) == 0 && b->layout != NULL &&
              (b->op_flags & OP_STRETCHY) &&
              (g_utf8_get_char (b->op) == '|' || g_utf8_get_char (b->op) == 0x2223) &&
              any_plain && asc + desc > (b->asc + b->desc) * 1.2)
            stretch_delim (env, b, asc, desc, st_em (st));
        }
      /* A delimiter asked for at a size: grown to it about the axis. */
      if (b->minsize > 0.0 && b->layout != NULL && b->minsize > b->asc + b->desc)
        {
          double axis = env->axis * st_em (st);

          stretch_delim (env, b, axis + b->minsize / 2.0, b->minsize / 2.0 - axis, st_em (st));
        }
      x += l;
      box_put (row, b, x, 0.0);
      x += b->w + r;
    }
  g_ptr_array_free (boxes, TRUE);
  row->w = x;
  box_fit (row);
  /* A row of one operator is that operator to the row it is in. */
  if (row->kids->len == 1)
    {
      W42MathBox *only = g_ptr_array_index (row->kids, 0);

      row->op = g_strdup (only->op);
      row->op_flags = only->op_flags & ~OP_STRETCHY;
      row->italic = only->italic;
      row->token = only->token;
    }
  return row;
}

/* msub, msup and msubsup: `sub` and `sup` either may be NULL. */
static W42MathBox *
lay_scripts (Env *env, W42MathBox *base, W42MathBox *sub, W42MathBox *sup, const St *st,
             double sub_em)
{
  W42MathBox *b = box_new ();
  double em = st_em (st), xh = env->xheight * em, theta = 0.05 * em;
  double u = 0.0, v = 0.0;
  double x = base->w;

  if (!base->token)
    {
      u = base->asc - 0.386 * sub_em;
      v = base->desc + 0.05 * sub_em;
    }
  box_put (b, base, 0.0, 0.0);
  if (sup != NULL)
    u = MAX (u, MAX (st->display ? 0.413 * em : 0.363 * em, sup->desc + xh / 4.0));
  if (sub != NULL && sup == NULL)
    v = MAX (v, MAX (0.15 * em, sub->asc - 0.8 * xh));
  if (sub != NULL && sup != NULL)
    {
      v = MAX (v, 0.247 * em);
      if ((u - sup->desc) - (sub->asc - v) < 4.0 * theta)
        {
          double psi;

          v = 4.0 * theta - (u - sup->desc) + sub->asc;
          psi = 0.8 * xh - (u - sup->desc);
          if (psi > 0.0)
            {
              u += psi;
              v -= psi;
            }
        }
    }
  /* An integral's scripts go where its slant puts them: the upper out
   * past the hook at its top, the lower in under the one at its foot. */
  {
    double ic = base->italic, back = 0.0;

    if (base->op_flags & OP_INTEGRAL)
      {
        ic = MAX (ic, (st->display ? 0.3 : 0.12) * em);
        back = st->display ? 0.15 * em : 0.05 * em;
      }
    if (sup != NULL)
      box_put (b, sup, x + ic, -u);
    if (sub != NULL)
      box_put (b, sub, x - back, v);
    b->w = x + MAX (sup != NULL ? sup->w + ic : 0.0, sub != NULL ? sub->w - back : 0.0) + 0.05 * em;
  }
  box_fit (b);
  b->op = g_strdup (base->op);
  b->op_flags = base->op_flags & ~(OP_STRETCHY | OP_FENCE);
  return b;
}

/* A character that lies across what it goes over or under. */
static gunichar
lone_char (const W42MathBox *b)
{
  if (b == NULL || b->op == NULL || *b->op == '\0' ||
      *g_utf8_next_char (b->op) != '\0')
    return 0;
  return g_utf8_get_char (b->op);
}

/* Makes an operator lying over or under `width` points of a base a
 * drawing of that width, when it can be drawn. */
static void
stretch_across (W42MathBox *b, double width, double em)
{
  gunichar c = lone_char (b);
  double h;

  gboolean arrow = c == 0x2192 || c == 0x2190 || c == 0x2194 || c == 0x20D7;

  if (c == 0 || !hstretchy (c) || b->layout == NULL)
    return;
  /* A bar or an arrow is drawn to the base's width; a hat or a tilde
   * only when the glyph is narrower than what it covers. */
  if (width <= b->w * 1.1 && c != 0xAF && c != 0x203E && c != '_' && c != 0x2212 && !arrow)
    return;
  if (arrow)
    width = MAX (width, 0.5 * em);
  g_clear_object (&b->layout);
  switch (c)
    {
    case 0xAF: case 0x203E: case '_': case 0x2212: h = 0.12 * em; break;
    case 0x23DE: case 0x23DF: case 0x23DC: case 0x23DD: h = 0.3 * em; break;
    case '^': case 0x2C6: case 0x302: case 0x2C7: h = MIN (0.25 * em, width * 0.15); break;
    case 0x2192: case 0x2190: case 0x2194: case 0x20D7: h = width < 1.5 * em ? 0.2 * em : 0.28 * em; break;
    default: h = 0.3 * em; break;
    }
  b->w = width;
  b->asc = h;
  b->desc = 0.0;
  {
    Draw d = { DRAW_HDELIM, c, 0.0, -h, width, 0.0, 0.05 * em, 0.0 };

    g_array_append_val (b->draws, d);
  }
}

/* munder, mover, munderover. */
static W42MathBox *
lay_underover (Env *env, const W42MathNode *n, const St *st)
{
  gboolean has_under = !g_str_equal (n->name, "mover");
  gboolean has_over = !g_str_equal (n->name, "munder");
  const W42MathNode *base_n = n->children->len > 0 ? g_ptr_array_index (n->children, 0) : NULL;
  const W42MathNode *under_n = has_under && n->children->len > 1 ? g_ptr_array_index (n->children, 1) : NULL;
  const W42MathNode *over_n = has_over ? (n->children->len > (has_under ? 2u : 1u)
                                          ? g_ptr_array_index (n->children, has_under ? 2 : 1) : NULL) : NULL;
  const W42MathNode *core = core_operator (base_n);
  W42MathBox *base, *under = NULL, *over = NULL, *b;
  gboolean accent, accentunder;
  St sst;
  double em = st_em (st), w, gap = 0.1 * em, ignore;

  if (base_n == NULL)
    return box_new ();
  base = lay (env, base_n, st);
  accent = over_n != NULL && attr_true (n, "accent", core_operator (over_n) != NULL &&
                                        (op_lookup (node_text (core_operator (over_n)), FORM_POSTFIX,
                                                    &ignore, &ignore) & OP_ACCENT));
  accentunder = under_n != NULL && attr_true (n, "accentunder", core_operator (under_n) != NULL &&
                                              (op_lookup (node_text (core_operator (under_n)), FORM_POSTFIX,
                                                          &ignore, &ignore) & OP_ACCENT));

  /* ∑ with its limits in the line: set to the side, as scripts. */
  if (core != NULL && !st->display && (base->op_flags & OP_MOVABLE) &&
      attr_true (core, "movablelimits", TRUE) && !accent && !accentunder)
    {
      St s1 = st_script (st, 1);

      if (under_n != NULL) under = lay (env, under_n, &s1);
      if (over_n != NULL) over = lay (env, over_n, &s1);
      return lay_scripts (env, base, under, over, st, s1.size);
    }

  sst = st_script (st, 1);
  if (over_n != NULL)
    {
      St os = accent ? *st : sst;

      if (accent) os.display = FALSE;
      over = lay (env, over_n, &os);
    }
  if (under_n != NULL)
    {
      St us = accentunder ? *st : sst;

      if (accentunder) us.display = FALSE;
      under = lay (env, under_n, &us);
    }

  w = base->w;
  if (over != NULL) stretch_across (over, base->w, em);
  if (under != NULL) stretch_across (under, base->w, em);
  if (over != NULL) w = MAX (w, over->w);
  if (under != NULL) w = MAX (w, under->w);

  b = box_new ();
  box_put (b, base, (w - base->w) / 2.0, 0.0);
  if (over != NULL)
    {
      /* An accent sits just over the letter, leaning with it; a
       * spacing accent's glyph is drawn high over its baseline, which
       * goes down into the letter so that the ink sits on top of it. */
      double y = -(base->asc + gap + over->desc);
      double skew = accent && base->token ? base->italic * 0.5 : 0.0;

      if (accent)
        y = -(base->asc + 0.05 * em) - (over->layout != NULL ? over->ink_low : 0.0);
      box_put (b, over, (w - over->w) / 2.0 + skew, y);
    }
  if (under != NULL)
    box_put (b, under, (w - under->w) / 2.0,
             base->desc + (accentunder ? 0.04 * em : gap) + under->asc);
  b->w = w;
  box_fit (b);
  if (base->op != NULL)
    {
      b->op = g_strdup (base->op);
      b->op_flags = base->op_flags & ~(OP_STRETCHY | OP_FENCE);
    }
  b->token = base->token && (accent || accentunder);
  b->italic = base->italic;
  return b;
}

static W42MathBox *
lay_frac (Env *env, const W42MathNode *n, const St *st)
{
  St s = st->display ? *st : st_script (st, 1);
  W42MathBox *num, *den, *b = box_new ();
  double em = st_em (st), axis = env->axis * em;
  double theta = 0.05 * em, u, v, phi, w, pad = 0.12 * em;
  const char *lt = w42_math_node_attr (n, "linethickness");
  const char *numalign = w42_math_node_attr (n, "numalign");
  const char *denalign = w42_math_node_attr (n, "denomalign");

  s.display = FALSE;
  num = n->children->len > 0 ? lay (env, g_ptr_array_index (n->children, 0), &s) : box_new ();
  den = n->children->len > 1 ? lay (env, g_ptr_array_index (n->children, 1), &s) : box_new ();

  if (lt != NULL)
    {
      if (g_ascii_strcasecmp (lt, "thin") == 0)
        theta *= 0.5;
      else if (g_ascii_strcasecmp (lt, "thick") == 0)
        theta *= 2.0;
      else if (g_ascii_strcasecmp (lt, "medium") != 0)
        {
          char *end;
          double f = g_ascii_strtod (lt, &end);

          theta = (*end == '\0') ? f * theta : parse_length (lt, em, theta * 100.0, theta);
        }
    }
  theta = MAX (theta, 0.0);

  if (st->display)
    {
      u = 0.677 * em;
      v = 0.686 * em;
      phi = theta > 0.0 ? 3.0 * theta : 7.0 * 0.05 * em;
    }
  else
    {
      u = 0.394 * em;
      v = 0.345 * em;
      phi = theta > 0.0 ? theta : 3.0 * 0.05 * em;
    }
  if (theta > 0.0)
    {
      u = MAX (u, axis + theta / 2.0 + phi + num->desc);
      v = MAX (v, den->asc + phi - axis + theta / 2.0);
    }
  else
    {
      double clear = (u - num->desc) - (den->asc - v);

      if (clear < phi)
        {
          u += (phi - clear) / 2.0;
          v += (phi - clear) / 2.0;
        }
    }

  w = MAX (num->w, den->w);
  {
    double nx = pad + (w - num->w) / 2.0, dx = pad + (w - den->w) / 2.0;

    if (numalign != NULL && g_str_equal (numalign, "left")) nx = pad;
    if (numalign != NULL && g_str_equal (numalign, "right")) nx = pad + w - num->w;
    if (denalign != NULL && g_str_equal (denalign, "left")) dx = pad;
    if (denalign != NULL && g_str_equal (denalign, "right")) dx = pad + w - den->w;
    box_put (b, num, nx, -u);
    box_put (b, den, dx, v);
  }
  b->w = w + 2.0 * pad;
  if (theta > 0.0)
    box_draw (b, DRAW_RECT, pad * 0.5, -axis - theta / 2.0, b->w - pad * 0.5, -axis + theta / 2.0, 0.0);
  box_fit (b);
  b->asc = MAX (b->asc, axis + theta / 2.0);
  return b;
}

static W42MathBox *
lay_radical (Env *env, W42MathBox *body, W42MathBox *index, const St *st)
{
  W42MathBox *b = box_new ();
  double em = st_em (st), theta = 0.05 * em;
  double phi = theta + 0.25 * (st->display ? env->xheight * em : theta);
  double top = -(body->asc + phi + theta), bottom = body->desc + 0.08 * em;
  double h = bottom - top;
  double sign_w = 0.55 * em + MIN (h * 0.05, 0.4 * em);
  double shift = 0.0;

  if (index != NULL)
    {
      /* The index sits in the crook of the sign, its foot at 60 per cent
       * of the sign's height; a wide one pushes the sign along. */
      double ix = MAX (0.0, 0.42 * sign_w - index->w);

      shift = MAX (0.0, index->w - 0.42 * sign_w);
      box_put (b, index, ix, bottom - 0.6 * h - index->desc);
    }
  box_put (b, body, shift + sign_w + 0.08 * em, 0.0);
  b->w = shift + sign_w + 0.08 * em + body->w + 0.1 * em;
  {
    Draw d = { DRAW_RADICAL, 0, shift, top + theta / 2.0, b->w - 0.02 * em, bottom, theta, shift + sign_w };

    g_array_append_val (b->draws, d);
  }
  box_fit (b);
  b->asc = MAX (b->asc, -top);
  b->desc = MAX (b->desc, bottom);
  return b;
}

static void
draw_radical (cairo_t *cr, const Draw *d)
{
  double x0 = d->x0, top = d->y0, bottom = d->y1, xs = d->extra, t = d->width;
  double h = bottom - top, w = xs - x0;

  cairo_save (cr);
  cairo_set_line_join (cr, CAIRO_LINE_JOIN_ROUND);
  cairo_set_line_cap (cr, CAIRO_LINE_CAP_BUTT);
  /* The tick and the long stroke thin, the stroke down thick, the bar
   * as thick as a fraction's. */
  cairo_set_line_width (cr, t * 0.9);
  cairo_move_to (cr, x0 + w * 0.02, bottom - h * 0.38);
  cairo_line_to (cr, x0 + w * 0.2, bottom - h * 0.46);
  cairo_stroke (cr);
  cairo_set_line_width (cr, t * 2.0);
  cairo_move_to (cr, x0 + w * 0.2, bottom - h * 0.46);
  cairo_line_to (cr, x0 + w * 0.5, bottom);
  cairo_stroke (cr);
  cairo_set_line_width (cr, t);
  cairo_move_to (cr, x0 + w * 0.5, bottom);
  cairo_line_to (cr, xs, top);
  cairo_line_to (cr, d->x1, top);
  cairo_stroke (cr);
  cairo_restore (cr);
}

static W42MathBox *
lay_table (Env *env, const W42MathNode *n, const St *st_in)
{
  St st = *st_in;
  double em = st_em (st_in), axis = env->axis * em;
  double colsp = parse_length (w42_math_node_attr (n, "columnspacing"), em, em, 0.8 * em);
  double rowsp = parse_length (w42_math_node_attr (n, "rowspacing"), em, em, 0.4 * em);
  const char *colalign = w42_math_node_attr (n, "columnalign");
  const char *frame = w42_math_node_attr (n, "frame");
  const char *rowlines = w42_math_node_attr (n, "rowlines");
  const char *collines = w42_math_node_attr (n, "columnlines");
  char **aligns = colalign != NULL ? g_strsplit (colalign, " ", -1) : NULL;
  GPtrArray *rows = g_ptr_array_new ();          /* GPtrArray* of W42MathBox* */
  GArray *colw = g_array_new (FALSE, TRUE, sizeof (double));
  GArray *rasc = g_array_new (FALSE, TRUE, sizeof (double));
  GArray *rdesc = g_array_new (FALSE, TRUE, sizeof (double));
  W42MathBox *b = box_new ();
  double total = 0.0, y, width = 0.0;
  double fpad = frame != NULL && g_str_equal (frame, "solid") ? 0.2 * em : 0.0;

  st.display = FALSE;
  for (guint r = 0; r < n->children->len; r++)
    {
      const W42MathNode *row_n = g_ptr_array_index (n->children, r);
      GPtrArray *cells = g_ptr_array_new ();
      gboolean labeled = g_str_equal (row_n->name, "mlabeledtr");
      double a = 0.0, d = 0.0;

      if (!g_str_equal (row_n->name, "mtr") && !labeled)
        {
          /* Something that is not a row: a row of one cell. */
          g_ptr_array_add (cells, lay (env, row_n, &st));
        }
      else
        for (guint c = labeled ? 1 : 0; c < row_n->children->len; c++)
          {
            const W42MathNode *cell = g_ptr_array_index (row_n->children, c);
            St cs = st_from (cell, &st);

            g_ptr_array_add (cells, g_str_equal (cell->name, "mtd") ? lay_row (env, cell, 0, &cs)
                                                                    : lay (env, cell, &cs));
          }
      for (guint c = 0; c < cells->len; c++)
        {
          W42MathBox *cb = g_ptr_array_index (cells, c);

          if (colw->len <= c)
            g_array_set_size (colw, c + 1);
          g_array_index (colw, double, c) = MAX (g_array_index (colw, double, c), cb->w);
          a = MAX (a, cb->asc);
          d = MAX (d, cb->desc);
        }
      a = MAX (a, 0.7 * em);
      d = MAX (d, 0.25 * em);
      g_array_append_val (rasc, a);
      g_array_append_val (rdesc, d);
      g_ptr_array_add (rows, cells);
      total += a + d + (r > 0 ? rowsp : 0.0);
    }

  for (guint c = 0; c < colw->len; c++)
    width += g_array_index (colw, double, c) + (c > 0 ? colsp : 0.0);
  width += 2.0 * fpad;
  total += 2.0 * fpad;

  y = -(total / 2.0 + axis) + fpad;
  for (guint r = 0; r < rows->len; r++)
    {
      GPtrArray *cells = g_ptr_array_index (rows, r);
      double a = g_array_index (rasc, double, r), d = g_array_index (rdesc, double, r);
      double x = fpad;

      y += a;
      for (guint c = 0; c < cells->len; c++)
        {
          W42MathBox *cb = g_ptr_array_index (cells, c);
          double cw = g_array_index (colw, double, c);
          guint n_al = aligns != NULL ? g_strv_length (aligns) : 0;
          const char *al = n_al > 0 ? aligns[MIN (c, n_al - 1)] : NULL;
          double cx = x + (cw - cb->w) / 2.0;

          if (al != NULL && g_str_equal (al, "left")) cx = x;
          else if (al != NULL && g_str_equal (al, "right")) cx = x + cw - cb->w;
          box_put (b, cb, cx, y);
          x += cw + colsp;
          if (collines != NULL && g_str_has_prefix (collines, "solid") && c + 1 < cells->len)
            box_draw (b, DRAW_LINE, x - colsp / 2.0, -(total / 2.0 + axis) + fpad,
                      x - colsp / 2.0, total / 2.0 - axis - fpad, 0.05 * em);
        }
      y += d;
      if (rowlines != NULL && g_str_has_prefix (rowlines, "solid") && r + 1 < rows->len)
        box_draw (b, DRAW_LINE, fpad, y + rowsp / 2.0, width - fpad, y + rowsp / 2.0, 0.05 * em);
      y += rowsp;
      g_ptr_array_free (cells, TRUE);
    }
  if (fpad > 0.0)
    {
      double t = -(total / 2.0 + axis), btm = total / 2.0 - axis;

      box_draw (b, DRAW_LINE, 0, t, width, t, 0.05 * em);
      box_draw (b, DRAW_LINE, 0, btm, width, btm, 0.05 * em);
      box_draw (b, DRAW_LINE, 0, t, 0, btm, 0.05 * em);
      box_draw (b, DRAW_LINE, width, t, width, btm, 0.05 * em);
    }
  b->w = width;
  box_fit (b);
  b->asc = MAX (b->asc, total / 2.0 + axis);
  b->desc = MAX (b->desc, total / 2.0 - axis);
  g_ptr_array_free (rows, TRUE);
  g_array_free (colw, TRUE);
  g_array_free (rasc, TRUE);
  g_array_free (rdesc, TRUE);
  g_strfreev (aligns);
  return b;
}

/* mfenced, an older way of writing brackets round a list: made the row
 * of operators and arguments it stands for. */
static W42MathBox *
lay_fenced (Env *env, const W42MathNode *n, const St *st)
{
  const char *open = w42_math_node_attr (n, "open");
  const char *close = w42_math_node_attr (n, "close");
  const char *seps = w42_math_node_attr (n, "separators");
  W42MathNode *row = w42_math_node_new ("mrow", NULL);
  W42MathBox *b;
  const char *sep = seps != NULL ? seps : ",";

  w42_math_node_add (row, w42_math_node_new ("mo", open != NULL ? open : "("));
  for (guint i = 0; i < n->children->len; i++)
    {
      const W42MathNode *kid = g_ptr_array_index (n->children, i);
      char *copy = w42_math_node_to_string (kid);
      W42MathNode *again = w42_math_parse (copy, -1, NULL);

      g_free (copy);
      if (again != NULL)
        {
          /* w42_math_parse wraps a fragment in a <math>: unwrap it. */
          W42MathNode *inner = again->children->len == 1
                                 ? g_ptr_array_steal_index (again->children, 0) : NULL;

          w42_math_node_free (again);
          if (inner != NULL)
            w42_math_node_add (row, inner);
        }
      if (i + 1 < n->children->len && *sep != '\0')
        {
          char one[8] = { 0 };
          gunichar c;

          while (*sep == ' ')
            sep++;
          c = g_utf8_get_char (sep);
          g_unichar_to_utf8 (c, one);
          w42_math_node_add (row, w42_math_node_new ("mo", one));
          if (*g_utf8_next_char (sep) != '\0')
            sep = g_utf8_next_char (sep);
        }
    }
  w42_math_node_add (row, w42_math_node_new ("mo", close != NULL ? close : ")"));
  b = lay_row (env, row, 0, st);
  w42_math_node_free (row);
  return b;
}

static W42MathBox *
lay_enclose (Env *env, const W42MathNode *n, const St *st)
{
  const char *notation = w42_math_node_attr (n, "notation");
  char **notes = g_strsplit (notation != NULL ? notation : "longdiv", " ", -1);
  W42MathBox *body = lay_row (env, n, 0, st);
  W42MathBox *b;
  double em = st_em (st), pad = 0.15 * em, t = 0.05 * em;
  double x0, y0, x1, y1;

  for (guint i = 0; notes[i] != NULL; i++)
    if (g_str_equal (notes[i], "radical"))
      {
        g_strfreev (notes);
        return lay_radical (env, body, NULL, st);
      }
  b = box_new ();
  box_put (b, body, pad, 0.0);
  b->w = body->w + 2.0 * pad;
  x0 = 0.0;
  x1 = b->w;
  y0 = -(body->asc + pad);
  y1 = body->desc + pad;
  for (guint i = 0; notes[i] != NULL; i++)
    {
      const char *k = notes[i];

      if (g_str_equal (k, "box") || g_str_equal (k, "roundedbox"))
        {
          box_draw (b, DRAW_LINE, x0, y0, x1, y0, t);
          box_draw (b, DRAW_LINE, x0, y1, x1, y1, t);
          box_draw (b, DRAW_LINE, x0, y0, x0, y1, t);
          box_draw (b, DRAW_LINE, x1, y0, x1, y1, t);
        }
      else if (g_str_equal (k, "circle"))
        box_draw (b, DRAW_ELLIPSE, x0, y0, x1, y1, t);
      else if (g_str_equal (k, "top") || g_str_equal (k, "actuarial"))
        box_draw (b, DRAW_LINE, x0, y0, x1, y0, t);
      else if (g_str_equal (k, "bottom"))
        box_draw (b, DRAW_LINE, x0, y1, x1, y1, t);
      else if (g_str_equal (k, "left") || g_str_equal (k, "longdiv"))
        box_draw (b, DRAW_LINE, x0, y0, x0, y1, t);
      if (g_str_equal (k, "right") || g_str_equal (k, "actuarial"))
        box_draw (b, DRAW_LINE, x1, y0, x1, y1, t);
      if (g_str_equal (k, "longdiv"))
        box_draw (b, DRAW_LINE, x0, y0, x1, y0, t);
      else if (g_str_equal (k, "updiagonalstrike"))
        box_draw (b, DRAW_LINE, x0, y1, x1, y0, t);
      else if (g_str_equal (k, "downdiagonalstrike"))
        box_draw (b, DRAW_LINE, x0, y0, x1, y1, t);
      else if (g_str_equal (k, "horizontalstrike"))
        box_draw (b, DRAW_LINE, x0, (y0 + y1) / 2.0, x1, (y0 + y1) / 2.0, t);
      else if (g_str_equal (k, "verticalstrike"))
        box_draw (b, DRAW_LINE, (x0 + x1) / 2.0, y0, (x0 + x1) / 2.0, y1, t);
    }
  g_strfreev (notes);
  box_fit (b);
  b->asc = MAX (b->asc, -y0 + t);
  b->desc = MAX (b->desc, y1 + t);
  return b;
}

/* mmultiscripts: the base, then its scripts after it in pairs, and after
 * an <mprescripts/> the pairs before it. */
static W42MathBox *
lay_multiscripts (Env *env, const W42MathNode *n, const St *st)
{
  St s1 = st_script (st, 1);
  W42MathBox *base, *pre = NULL, *b;
  gboolean prescripts = FALSE;
  GPtrArray *post_sub = g_ptr_array_new (), *post_sup = g_ptr_array_new ();
  GPtrArray *pre_sub = g_ptr_array_new (), *pre_sup = g_ptr_array_new ();

  if (n->children->len == 0)
    return box_new ();
  base = lay (env, g_ptr_array_index (n->children, 0), st);
  for (guint i = 1; i < n->children->len; i++)
    {
      const W42MathNode *k = g_ptr_array_index (n->children, i);

      if (g_str_equal (k->name, "mprescripts"))
        {
          prescripts = TRUE;
          continue;
        }
      {
        GPtrArray *subs = prescripts ? pre_sub : post_sub;
        GPtrArray *sups = prescripts ? pre_sup : post_sup;
        GPtrArray *into = subs->len == sups->len ? subs : sups;

        g_ptr_array_add (into, g_str_equal (k->name, "none") ? NULL : lay (env, k, &s1));
      }
    }

  /* Each side's scripts stacked in two rows, the pairs side by side. */
  {
    W42MathBox *sub_row = NULL, *sup_row = NULL;

    for (guint side = 0; side < 2; side++)
      {
        GPtrArray *subs = side == 0 ? pre_sub : post_sub, *sups = side == 0 ? pre_sup : post_sup;
        W42MathBox *sr = box_new (), *pr = box_new ();
        double x = 0.0;

        for (guint i = 0; i < MAX (subs->len, sups->len); i++)
          {
            W42MathBox *sb = i < subs->len ? g_ptr_array_index (subs, i) : NULL;
            W42MathBox *pb = i < sups->len ? g_ptr_array_index (sups, i) : NULL;
            double w = MAX (sb != NULL ? sb->w : 0.0, pb != NULL ? pb->w : 0.0);

            if (sb != NULL) box_put (sr, sb, x + (side == 0 ? w - sb->w : 0.0), 0.0);
            if (pb != NULL) box_put (pr, pb, x + (side == 0 ? w - pb->w : 0.0), 0.0);
            x += w + 0.05 * st_em (st);
          }
        sr->w = pr->w = x;
        box_fit (sr);
        box_fit (pr);
        if (side == 0)
          {
            if (subs->len + sups->len > 0)
              {
                W42MathBox *e = box_new ();

                pre = lay_scripts (env, e, sr->kids->len > 0 ? sr : NULL,
                                   pr->kids->len > 0 ? pr : NULL, st, s1.size);
                if (sr->kids->len == 0) w42_math_box_free (sr);
                if (pr->kids->len == 0) w42_math_box_free (pr);
              }
            else
              {
                w42_math_box_free (sr);
                w42_math_box_free (pr);
              }
          }
        else
          {
            sub_row = sr;
            sup_row = pr;
          }
      }
    if (sub_row->kids->len == 0) g_clear_pointer (&sub_row, w42_math_box_free);
    if (sup_row->kids->len == 0) g_clear_pointer (&sup_row, w42_math_box_free);
    base = lay_scripts (env, base, sub_row, sup_row, st, s1.size);
  }
  g_ptr_array_free (post_sub, TRUE);
  g_ptr_array_free (post_sup, TRUE);
  g_ptr_array_free (pre_sub, TRUE);
  g_ptr_array_free (pre_sup, TRUE);

  b = box_new ();
  if (pre != NULL)
    {
      box_put (b, pre, 0.0, 0.0);
      box_put (b, base, pre->w, 0.0);
      b->w = pre->w + base->w;
    }
  else
    {
      box_put (b, base, 0.0, 0.0);
      b->w = base->w;
    }
  box_fit (b);
  return b;
}

static W42MathBox *
lay (Env *env, const W42MathNode *n, const St *st_in)
{
  St st = st_from (n, st_in);
  const char *name = n->name;
  W42MathBox *b;

  if (is_token (name))
    return lay_token (env, n, st_in);

  if (g_str_equal (name, "mglyph"))
    {
      const char *alt = w42_math_node_attr (n, "alt");

      return text_box (env, &st, alt != NULL ? alt : "", MATH_FAMILY, FALSE, FALSE, st.size);
    }
  if (g_str_equal (name, "mspace"))
    {
      b = box_new ();
      b->w = parse_length (w42_math_node_attr (n, "width"), st.size, 0.0, 0.0);
      b->asc = parse_length (w42_math_node_attr (n, "height"), st.size, 0.0, 0.0);
      b->desc = parse_length (w42_math_node_attr (n, "depth"), st.size, 0.0, 0.0);
      return b;
    }
  if (g_str_equal (name, "msub") || g_str_equal (name, "msup") || g_str_equal (name, "msubsup"))
    {
      St s1 = st_script (&st, 1);
      W42MathBox *base, *sub = NULL, *sup = NULL;

      if (n->children->len == 0)
        return box_new ();
      base = lay (env, g_ptr_array_index (n->children, 0), &st);
      if (g_str_equal (name, "msup"))
        sup = n->children->len > 1 ? lay (env, g_ptr_array_index (n->children, 1), &s1) : NULL;
      else
        {
          sub = n->children->len > 1 ? lay (env, g_ptr_array_index (n->children, 1), &s1) : NULL;
          if (g_str_equal (name, "msubsup") && n->children->len > 2)
            sup = lay (env, g_ptr_array_index (n->children, 2), &s1);
        }
      return lay_scripts (env, base, sub, sup, &st, s1.size);
    }
  if (g_str_equal (name, "munder") || g_str_equal (name, "mover") || g_str_equal (name, "munderover"))
    return lay_underover (env, n, &st);
  if (g_str_equal (name, "mfrac"))
    {
      const char *bevelled = w42_math_node_attr (n, "bevelled");

      if (bevelled != NULL && g_str_equal (bevelled, "true") && n->children->len == 2)
        {
          W42MathNode *row = w42_math_node_new ("mrow", NULL);
          char *a = w42_math_node_to_string (g_ptr_array_index (n->children, 0));
          char *c = w42_math_node_to_string (g_ptr_array_index (n->children, 1));
          W42MathNode *an = w42_math_parse (a, -1, NULL), *cn = w42_math_parse (c, -1, NULL);

          if (an != NULL) w42_math_node_add (row, an);
          w42_math_node_add (row, w42_math_node_new ("mo", "/"));
          if (cn != NULL) w42_math_node_add (row, cn);
          b = lay_row (env, row, 0, &st);
          w42_math_node_free (row);
          g_free (a);
          g_free (c);
          return b;
        }
      return lay_frac (env, n, &st);
    }
  if (g_str_equal (name, "msqrt"))
    return lay_radical (env, lay_row (env, n, 0, &st), NULL, &st);
  if (g_str_equal (name, "mroot"))
    {
      St s2 = st_script (&st, 2);
      W42MathBox *body = n->children->len > 0 ? lay (env, g_ptr_array_index (n->children, 0), &st)
                                              : box_new ();
      W42MathBox *index = n->children->len > 1 ? lay (env, g_ptr_array_index (n->children, 1), &s2)
                                               : NULL;

      return lay_radical (env, body, index, &st);
    }
  if (g_str_equal (name, "mtable"))
    return lay_table (env, n, &st);
  if (g_str_equal (name, "mfenced"))
    return lay_fenced (env, n, &st);
  if (g_str_equal (name, "menclose"))
    return lay_enclose (env, n, &st);
  if (g_str_equal (name, "mmultiscripts"))
    return lay_multiscripts (env, n, &st);
  if (g_str_equal (name, "semantics") || g_str_equal (name, "maction"))
    {
      /* What is shown is the first child; the rest describe it. */
      guint pick = 0;
      const char *sel = w42_math_node_attr (n, "selection");

      if (g_str_equal (name, "maction") && sel != NULL)
        pick = (guint) MAX (g_ascii_strtoll (sel, NULL, 10) - 1, 0);
      if (pick < n->children->len)
        return lay (env, g_ptr_array_index (n->children, pick), &st);
      return box_new ();
    }
  if (g_str_equal (name, "annotation") || g_str_equal (name, "annotation-xml") ||
      g_str_equal (name, "none") || g_str_equal (name, "mprescripts"))
    return box_new ();

  b = lay_row (env, n, 0, &st);
  if (g_str_equal (name, "mphantom"))
    b->hidden = TRUE;
  else if (g_str_equal (name, "merror"))
    {
      b->has_color = TRUE;
      b->color = 0xCC0000;
    }
  else if (g_str_equal (name, "mpadded"))
    {
      const char *v;

      if ((v = w42_math_node_attr (n, "width")) != NULL)
        b->w = MAX ((v[0] == '+' || v[0] == '-') ? b->w + parse_length (v, st.size, b->w, 0.0)
                                                 : parse_length (v, st.size, b->w, b->w), 0.0);
      if ((v = w42_math_node_attr (n, "height")) != NULL)
        b->asc = (v[0] == '+' || v[0] == '-') ? b->asc + parse_length (v, st.size, b->asc, 0.0)
                                              : parse_length (v, st.size, b->asc, b->asc);
      if ((v = w42_math_node_attr (n, "depth")) != NULL)
        b->desc = (v[0] == '+' || v[0] == '-') ? b->desc + parse_length (v, st.size, b->desc, 0.0)
                                               : parse_length (v, st.size, b->desc, b->desc);
    }
  if (st.has_color && !b->has_color)
    {
      b->has_color = TRUE;
      b->color = st.color;
    }
  return b;
}

W42MathBox *
w42_math_box_new (const W42MathNode *root, double size)
{
  Env env;
  St st = { 0 };
  W42MathBox *b;
  cairo_font_options_t *opts = cairo_font_options_create ();

  g_return_val_if_fail (root != NULL, NULL);

  /* Points throughout: a context at 72 to the inch, its metrics not
   * hinted, so that the formula is the same shape at every zoom. */
  env.ctx = pango_font_map_create_context (pango_cairo_font_map_get_default ());
  pango_cairo_context_set_resolution (env.ctx, 72.0);
  cairo_font_options_set_hint_metrics (opts, CAIRO_HINT_METRICS_OFF);
  cairo_font_options_set_hint_style (opts, CAIRO_HINT_STYLE_NONE);
  pango_cairo_context_set_font_options (env.ctx, opts);
  cairo_font_options_destroy (opts);

  st.size = st.base = MAX (size, 1.0);
  st.display = w42_math_is_display (root);

  /* The axis is where a minus sign's middle is, and the x-height the
   * top of an x: from the faces the formula will be set in. */
  {
    W42MathBox *minus = text_box (&env, &st, "\342\210\222", MATH_FAMILY, FALSE, FALSE, 100.0);
    W42MathBox *x = text_box (&env, &st, "x", MATH_FAMILY, FALSE, TRUE, 100.0);

    env.axis = minus->asc > 0.0 ? CLAMP ((minus->asc - minus->ink_low) / 200.0, 0.15, 0.35) : 0.25;
    env.xheight = x->asc > 0.0 ? CLAMP (x->asc / 100.0, 0.35, 0.6) : 0.45;
    w42_math_box_free (minus);
    w42_math_box_free (x);
  }

  {
    St top = st_from (root, &st);

    b = lay_row (&env, root, 0, &top);
  }
  b->size = st.base;
  g_object_unref (env.ctx);
  return b;
}

void
w42_math_box_extents (const W42MathBox *box, double *width, double *ascent, double *descent)
{
  if (width != NULL) *width = box != NULL ? box->w : 0.0;
  if (ascent != NULL) *ascent = box != NULL ? box->asc : 0.0;
  if (descent != NULL) *descent = box != NULL ? box->desc : 0.0;
}

static void
set_rgb (cairo_t *cr, guint32 rgb)
{
  cairo_set_source_rgb (cr, ((rgb >> 16) & 0xff) / 255.0, ((rgb >> 8) & 0xff) / 255.0,
                        (rgb & 0xff) / 255.0);
}

static void
draw_box (const W42MathBox *b, cairo_t *cr, double x, double y)
{
  if (b->hidden)
    return;
  cairo_save (cr);
  if (b->has_color)
    set_rgb (cr, b->color);
  if (b->layout != NULL)
    {
      cairo_move_to (cr, x + b->lx, y + b->ly);
      pango_cairo_show_layout (cr, b->layout);
      cairo_new_path (cr);
    }
  for (guint i = 0; i < b->draws->len; i++)
    {
      const Draw *d = &g_array_index (b->draws, Draw, i);
      Draw at = *d;

      at.x0 += x; at.x1 += x; at.y0 += y; at.y1 += y;
      if (d->kind == DRAW_RADICAL)
        at.extra += x;
      switch (d->kind)
        {
        case DRAW_LINE:
          cairo_set_line_width (cr, d->width);
          cairo_move_to (cr, at.x0, at.y0);
          cairo_line_to (cr, at.x1, at.y1);
          cairo_stroke (cr);
          break;
        case DRAW_RECT:
          cairo_rectangle (cr, at.x0, at.y0, at.x1 - at.x0, at.y1 - at.y0);
          cairo_fill (cr);
          break;
        case DRAW_ELLIPSE:
          cairo_save (cr);
          cairo_translate (cr, (at.x0 + at.x1) / 2.0, (at.y0 + at.y1) / 2.0);
          cairo_scale (cr, MAX ((at.x1 - at.x0) / 2.0, 0.1), MAX ((at.y1 - at.y0) / 2.0, 0.1));
          cairo_arc (cr, 0, 0, 1, 0, 2 * G_PI);
          cairo_restore (cr);
          cairo_set_line_width (cr, d->width);
          cairo_stroke (cr);
          break;
        case DRAW_DELIM:
          draw_delim (cr, &at);
          break;
        case DRAW_HDELIM:
          draw_hdelim (cr, &at);
          break;
        case DRAW_RADICAL:
          draw_radical (cr, &at);
          break;
        }
    }
  for (guint i = 0; i < b->kids->len; i++)
    draw_box (g_ptr_array_index (b->kids, i), cr,
              x + g_array_index (b->at, double, 2 * i),
              y + g_array_index (b->at, double, 2 * i + 1));
  cairo_restore (cr);
}

void
w42_math_box_draw (const W42MathBox *box, cairo_t *cr, double x, double y)
{
  g_return_if_fail (box != NULL && cr != NULL);
  draw_box (box, cr, x, y);
}

/* The air at each side of a formula, as a character has side bearings:
 * a twentieth of the size. */
#define SIDE 0.05

void
w42_math_box_draw_in (const W42MathBox *box, cairo_t *cr, double x, double y, double w, double h)
{
  double bw, sx, sy;

  g_return_if_fail (box != NULL && cr != NULL);
  bw = box->w + 2.0 * SIDE * box->size;
  if (bw <= 0.0 || box->asc + box->desc <= 0.0 || w <= 0.0 || h <= 0.0)
    return;
  sx = w / bw;
  sy = h / (box->asc + box->desc);
  cairo_save (cr);
  cairo_translate (cr, x, y);
  cairo_scale (cr, sx, sy);
  draw_box (box, cr, SIDE * box->size, box->asc);
  cairo_restore (cr);
}

gboolean
w42_math_measure (const char *mathml, double size, int *width, int *height, int *descent)
{
  W42MathNode *root = mathml != NULL ? w42_math_parse (mathml, -1, NULL) : NULL;
  W42MathBox *box;
  double w, a, d;

  if (root == NULL)
    return FALSE;
  box = w42_math_box_new (root, size);
  w42_math_box_extents (box, &w, &a, &d);
  w += 2.0 * SIDE * size;
  if (width) *width = MAX ((int) ceil (w * 20.0), 20);
  if (height) *height = MAX ((int) ceil ((a + d) * 20.0), 20);
  if (descent) *descent = MAX ((int) ceil (d * 20.0), 0);
  w42_math_box_free (box);
  w42_math_node_free (root);
  return TRUE;
}

GBytes *
w42_math_render_png (const char *mathml, double size, int ppi, int *pixel_w, int *pixel_h)
{
  W42MathNode *root = mathml != NULL ? w42_math_parse (mathml, -1, NULL) : NULL;
  W42MathBox *box;
  cairo_surface_t *surface;
  cairo_t *cr;
  double w, a, d, scale = MAX (ppi, 24) / 72.0;
  int pw, ph;
  GBytes *png;

  if (root == NULL)
    return NULL;
  box = w42_math_box_new (root, size);
  w42_math_box_extents (box, &w, &a, &d);
  w += 2.0 * SIDE * size;
  pw = MAX ((int) ceil (w * scale), 1);
  ph = MAX ((int) ceil ((a + d) * scale), 1);
  surface = cairo_image_surface_create (CAIRO_FORMAT_ARGB32, pw, ph);
  cr = cairo_create (surface);
  cairo_set_source_rgb (cr, 0, 0, 0);
  w42_math_box_draw_in (box, cr, 0, 0, pw, ph);
  cairo_destroy (cr);
  png = w42_image_surface_to_png (surface);
  cairo_surface_destroy (surface);
  w42_math_box_free (box);
  w42_math_node_free (root);
  if (pixel_w) *pixel_w = pw;
  if (pixel_h) *pixel_h = ph;
  return png;
}
