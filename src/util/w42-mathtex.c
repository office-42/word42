/* w42-mathtex.c - see w42-mathtex.h
 *
 * Copyright (C) 2026 Andreas Røsdal
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * The MathML made is what MathJax and the other TeX-to-MathML
 * converters make, so that a formula typed here reads in a browser as
 * one typed there: a letter an <mi> each, a number an <mn>, a function's
 * name an <mi> and U+2061 after it, a bracket typed plainly an
 * <mo stretchy="false"> that keeps its size and a \left one an <mo> that
 * grows, a sum with its limits an <munderover> that the line sets to the
 * side.
 */

#include "w42-mathtex.h"

#include <string.h>
#include <glib/gi18n.h>

typedef enum {
  K_MI,          /* an ordinary symbol: a letter, ∞, ∂ */
  K_MO,          /* an operator, a relation, an arrow */
  K_FENCE,       /* a bracket */
  K_BIG,         /* an integral: its limits at its side */
  K_BIGLIM,      /* a sum: its limits over and under on a line of its own */
  K_FUNC,        /* sin: a name, and function application after it */
  K_FUNCLIM      /* lim: the same, with limits under */
} Kind;

static const struct { const char *cmd; const char *utf8; Kind kind; } SYMBOLS[] = {
  { "alpha", "α", K_MI }, { "beta", "β", K_MI }, { "gamma", "γ", K_MI },
  { "delta", "δ", K_MI }, { "epsilon", "ϵ", K_MI }, { "varepsilon", "ε", K_MI },
  { "zeta", "ζ", K_MI }, { "eta", "η", K_MI }, { "theta", "θ", K_MI },
  { "vartheta", "ϑ", K_MI }, { "iota", "ι", K_MI }, { "kappa", "κ", K_MI },
  { "varkappa", "ϰ", K_MI }, { "lambda", "λ", K_MI }, { "mu", "μ", K_MI },
  { "nu", "ν", K_MI }, { "xi", "ξ", K_MI }, { "omicron", "ο", K_MI },
  { "pi", "π", K_MI }, { "varpi", "ϖ", K_MI }, { "rho", "ρ", K_MI },
  { "varrho", "ϱ", K_MI }, { "sigma", "σ", K_MI }, { "varsigma", "ς", K_MI },
  { "tau", "τ", K_MI }, { "upsilon", "υ", K_MI }, { "phi", "ϕ", K_MI },
  { "varphi", "φ", K_MI }, { "chi", "χ", K_MI }, { "psi", "ψ", K_MI },
  { "omega", "ω", K_MI }, { "digamma", "ϝ", K_MI },
  { "Gamma", "Γ", K_MI }, { "Delta", "Δ", K_MI }, { "Theta", "Θ", K_MI },
  { "Lambda", "Λ", K_MI }, { "Xi", "Ξ", K_MI }, { "Pi", "Π", K_MI },
  { "Sigma", "Σ", K_MI }, { "Upsilon", "Υ", K_MI }, { "Phi", "Φ", K_MI },
  { "Psi", "Ψ", K_MI }, { "Omega", "Ω", K_MI },

  { "infty", "∞", K_MI }, { "partial", "∂", K_MI }, { "nabla", "∇", K_MI },
  { "hbar", "ℏ", K_MI }, { "hslash", "ℏ", K_MI }, { "ell", "ℓ", K_MI },
  { "wp", "℘", K_MI }, { "Re", "ℜ", K_MI }, { "Im", "ℑ", K_MI },
  { "aleph", "ℵ", K_MI }, { "beth", "ℶ", K_MI }, { "emptyset", "∅", K_MI },
  { "varnothing", "∅", K_MI }, { "forall", "∀", K_MI }, { "exists", "∃", K_MI },
  { "nexists", "∄", K_MI }, { "top", "⊤", K_MI }, { "bot", "⊥", K_MI },
  { "angle", "∠", K_MI }, { "triangle", "△", K_MI }, { "prime", "′", K_MI },
  { "backslash", "\\", K_MI }, { "surd", "√", K_MI }, { "flat", "♭", K_MI },
  { "sharp", "♯", K_MI }, { "natural", "♮", K_MI }, { "clubsuit", "♣", K_MI },
  { "diamondsuit", "♢", K_MI }, { "heartsuit", "♡", K_MI },
  { "spadesuit", "♠", K_MI }, { "imath", "ı", K_MI }, { "jmath", "ȷ", K_MI },
  { "complement", "∁", K_MI }, { "mho", "℧", K_MI }, { "Box", "□", K_MI },
  { "square", "□", K_MI }, { "blacksquare", "■", K_MI }, { "checkmark", "✓", K_MI },
  { "degree", "°", K_MI }, { "S", "§", K_MI }, { "P", "¶", K_MI },

  { "pm", "±", K_MO }, { "mp", "∓", K_MO }, { "times", "×", K_MO },
  { "div", "÷", K_MO }, { "cdot", "⋅", K_MO }, { "ast", "∗", K_MO },
  { "star", "⋆", K_MO }, { "circ", "∘", K_MO }, { "bullet", "∙", K_MO },
  { "cap", "∩", K_MO }, { "cup", "∪", K_MO }, { "sqcap", "⊓", K_MO },
  { "sqcup", "⊔", K_MO }, { "wedge", "∧", K_MO }, { "land", "∧", K_MO },
  { "vee", "∨", K_MO }, { "lor", "∨", K_MO }, { "oplus", "⊕", K_MO },
  { "ominus", "⊖", K_MO }, { "otimes", "⊗", K_MO }, { "oslash", "⊘", K_MO },
  { "odot", "⊙", K_MO }, { "setminus", "∖", K_MO }, { "smallsetminus", "∖", K_MO },
  { "uplus", "⊎", K_MO }, { "amalg", "⨿", K_MO }, { "diamond", "⋄", K_MO },
  { "bigtriangleup", "△", K_MO }, { "bigtriangledown", "▽", K_MO },
  { "triangleleft", "◃", K_MO }, { "triangleright", "▹", K_MO },
  { "dagger", "†", K_MO }, { "ddagger", "‡", K_MO }, { "wr", "≀", K_MO },
  { "neg", "¬", K_MO }, { "lnot", "¬", K_MO },

  { "le", "≤", K_MO }, { "leq", "≤", K_MO }, { "ge", "≥", K_MO },
  { "geq", "≥", K_MO }, { "leqslant", "⩽", K_MO }, { "geqslant", "⩾", K_MO },
  { "ne", "≠", K_MO }, { "neq", "≠", K_MO }, { "equiv", "≡", K_MO },
  { "approx", "≈", K_MO }, { "approxeq", "≊", K_MO }, { "sim", "∼", K_MO },
  { "simeq", "≃", K_MO }, { "cong", "≅", K_MO }, { "propto", "∝", K_MO },
  { "in", "∈", K_MO }, { "notin", "∉", K_MO }, { "ni", "∋", K_MO },
  { "owns", "∋", K_MO }, { "subset", "⊂", K_MO }, { "supset", "⊃", K_MO },
  { "subseteq", "⊆", K_MO }, { "supseteq", "⊇", K_MO }, { "subsetneq", "⊊", K_MO },
  { "supsetneq", "⊋", K_MO }, { "sqsubseteq", "⊑", K_MO }, { "sqsupseteq", "⊒", K_MO },
  { "perp", "⊥", K_MO }, { "parallel", "∥", K_MO }, { "mid", "∣", K_MO },
  { "nmid", "∤", K_MO }, { "ll", "≪", K_MO }, { "gg", "≫", K_MO },
  { "lll", "⋘", K_MO }, { "ggg", "⋙", K_MO }, { "prec", "≺", K_MO },
  { "succ", "≻", K_MO }, { "preceq", "⪯", K_MO }, { "succeq", "⪰", K_MO },
  { "vdash", "⊢", K_MO }, { "dashv", "⊣", K_MO }, { "models", "⊨", K_MO },
  { "doteq", "≐", K_MO }, { "asymp", "≍", K_MO }, { "bowtie", "⋈", K_MO },
  { "smile", "⌣", K_MO }, { "frown", "⌢", K_MO }, { "lesssim", "≲", K_MO },
  { "gtrsim", "≳", K_MO }, { "triangleq", "≜", K_MO }, { "coloneqq", "≔", K_MO },
  { "coloneq", "≔", K_MO },
  { "to", "→", K_MO }, { "rightarrow", "→", K_MO }, { "gets", "←", K_MO },
  { "leftarrow", "←", K_MO }, { "leftrightarrow", "↔", K_MO },
  { "Rightarrow", "⇒", K_MO }, { "Leftarrow", "⇐", K_MO },
  { "Leftrightarrow", "⇔", K_MO }, { "implies", "⟹", K_MO },
  { "impliedby", "⟸", K_MO }, { "iff", "⟺", K_MO }, { "mapsto", "↦", K_MO },
  { "longmapsto", "⟼", K_MO }, { "longrightarrow", "⟶", K_MO },
  { "longleftarrow", "⟵", K_MO }, { "longleftrightarrow", "⟷", K_MO },
  { "Longrightarrow", "⟹", K_MO }, { "Longleftarrow", "⟸", K_MO },
  { "Longleftrightarrow", "⟺", K_MO }, { "uparrow", "↑", K_MO },
  { "downarrow", "↓", K_MO }, { "updownarrow", "↕", K_MO },
  { "Uparrow", "⇑", K_MO }, { "Downarrow", "⇓", K_MO }, { "nearrow", "↗", K_MO },
  { "searrow", "↘", K_MO }, { "swarrow", "↙", K_MO }, { "nwarrow", "↖", K_MO },
  { "hookrightarrow", "↪", K_MO }, { "hookleftarrow", "↩", K_MO },
  { "rightharpoonup", "⇀", K_MO }, { "rightleftharpoons", "⇌", K_MO },
  { "leadsto", "⇝", K_MO }, { "therefore", "∴", K_MO }, { "because", "∵", K_MO },
  { "ldots", "…", K_MO }, { "dots", "…", K_MO }, { "dotsc", "…", K_MO },
  { "cdots", "⋯", K_MO }, { "dotsb", "⋯", K_MO }, { "vdots", "⋮", K_MO },
  { "ddots", "⋱", K_MO }, { "colon", ":", K_MO },

  { "langle", "⟨", K_FENCE }, { "rangle", "⟩", K_FENCE }, { "lceil", "⌈", K_FENCE },
  { "rceil", "⌉", K_FENCE }, { "lfloor", "⌊", K_FENCE }, { "rfloor", "⌋", K_FENCE },
  { "lbrace", "{", K_FENCE }, { "rbrace", "}", K_FENCE }, { "{", "{", K_FENCE },
  { "}", "}", K_FENCE }, { "vert", "|", K_FENCE }, { "Vert", "‖", K_FENCE },
  { "|", "‖", K_FENCE }, { "lvert", "|", K_FENCE }, { "rvert", "|", K_FENCE },
  { "lVert", "‖", K_FENCE }, { "rVert", "‖", K_FENCE }, { "lbrack", "[", K_FENCE },
  { "rbrack", "]", K_FENCE }, { "llbracket", "⟦", K_FENCE }, { "rrbracket", "⟧", K_FENCE },

  { "sum", "∑", K_BIGLIM }, { "prod", "∏", K_BIGLIM }, { "coprod", "∐", K_BIGLIM },
  { "bigcup", "⋃", K_BIGLIM }, { "bigcap", "⋂", K_BIGLIM },
  { "bigoplus", "⨁", K_BIGLIM }, { "bigotimes", "⨂", K_BIGLIM },
  { "bigodot", "⨀", K_BIGLIM }, { "bigwedge", "⋀", K_BIGLIM },
  { "bigvee", "⋁", K_BIGLIM }, { "biguplus", "⨄", K_BIGLIM },
  { "bigsqcup", "⨆", K_BIGLIM },
  { "int", "∫", K_BIG }, { "iint", "∬", K_BIG }, { "iiint", "∭", K_BIG },
  { "oint", "∮", K_BIG }, { "oiint", "∯", K_BIG },

  { "arccos", "arccos", K_FUNC }, { "arcsin", "arcsin", K_FUNC },
  { "arctan", "arctan", K_FUNC }, { "arg", "arg", K_FUNC }, { "cos", "cos", K_FUNC },
  { "cosh", "cosh", K_FUNC }, { "cot", "cot", K_FUNC }, { "coth", "coth", K_FUNC },
  { "csc", "csc", K_FUNC }, { "deg", "deg", K_FUNC }, { "dim", "dim", K_FUNC },
  { "exp", "exp", K_FUNC }, { "hom", "hom", K_FUNC }, { "ker", "ker", K_FUNC },
  { "lg", "lg", K_FUNC }, { "ln", "ln", K_FUNC }, { "log", "log", K_FUNC },
  { "sec", "sec", K_FUNC }, { "sin", "sin", K_FUNC }, { "sinh", "sinh", K_FUNC },
  { "tan", "tan", K_FUNC }, { "tanh", "tanh", K_FUNC },
  { "det", "det", K_FUNCLIM }, { "gcd", "gcd", K_FUNCLIM }, { "inf", "inf", K_FUNCLIM },
  { "lim", "lim", K_FUNCLIM }, { "liminf", "lim inf", K_FUNCLIM },
  { "limsup", "lim sup", K_FUNCLIM }, { "max", "max", K_FUNCLIM },
  { "min", "min", K_FUNCLIM }, { "Pr", "Pr", K_FUNCLIM }, { "sup", "sup", K_FUNCLIM },
};

/* \hat and the rest: the character, and whether it goes under. */
static const struct { const char *cmd; const char *utf8; gboolean under; } ACCENTS[] = {
  { "hat", "^", FALSE }, { "widehat", "^", FALSE }, { "check", "ˇ", FALSE },
  { "tilde", "~", FALSE }, { "widetilde", "~", FALSE }, { "acute", "´", FALSE },
  { "grave", "`", FALSE }, { "dot", "˙", FALSE }, { "ddot", "¨", FALSE },
  { "breve", "˘", FALSE }, { "bar", "¯", FALSE }, { "overline", "¯", FALSE },
  { "vec", "→", FALSE }, { "overrightarrow", "→", FALSE },
  { "overleftarrow", "←", FALSE }, { "overleftrightarrow", "↔", FALSE },
  { "underline", "_", TRUE },
};

static const struct { const char *cmd; const char *variant; } ALPHABETS[] = {
  { "mathrm", "normal" }, { "mathup", "normal" }, { "mathbf", "bold" },
  { "mathit", "italic" }, { "mathbb", "double-struck" }, { "mathcal", "script" },
  { "mathscr", "script" }, { "mathfrak", "fraktur" }, { "mathsf", "sans-serif" },
  { "mathtt", "monospace" }, { "boldsymbol", "bold-italic" }, { "bm", "bold-italic" },
};

static const struct { const char *cmd; const char *em; } SPACES[] = {
  { ",", "0.1667em" }, { "thinspace", "0.1667em" }, { ":", "0.2222em" },
  { ">", "0.2222em" }, { "medspace", "0.2222em" }, { ";", "0.2778em" },
  { "thickspace", "0.2778em" }, { "!", "-0.1667em" }, { "negthinspace", "-0.1667em" },
  { "quad", "1em" }, { "qquad", "2em" }, { " ", "0.25em" }, { "enspace", "0.5em" },
};

static const struct { const char *cmd; const char *size; } BIGS[] = {
  { "big", "1.2em" }, { "Big", "1.623em" }, { "bigg", "2.047em" }, { "Bigg", "2.470em" },
  { "bigl", "1.2em" }, { "Bigl", "1.623em" }, { "biggl", "2.047em" }, { "Biggl", "2.470em" },
  { "bigr", "1.2em" }, { "Bigr", "1.623em" }, { "biggr", "2.047em" }, { "Biggr", "2.470em" },
  { "bigm", "1.2em" }, { "Bigm", "1.623em" }, { "biggm", "2.047em" }, { "Biggm", "2.470em" },
};

/* ---------------------------------------------------------------------- */
/* LaTeX to MathML                                                         */
/* ---------------------------------------------------------------------- */

typedef enum {
  STOP_END,          /* the end of the text */
  STOP_BRACE,        /* } */
  STOP_RIGHT,        /* \right */
  STOP_ENV,          /* \end{...} */
  STOP_AMP,          /* & */
  STOP_ROW           /* \\ */
} Stop;

typedef struct {
  const char *s;
  gsize       i, len;
  GError    **error;
  gboolean    failed;
  char       *end_name;      /* the environment a STOP_ENV ended */
} Tex;

static W42MathNode *
mk (const char *name, const char *text)
{
  return w42_math_node_new (name, text);
}

static W42MathNode *
mk2 (const char *name, W42MathNode *a, W42MathNode *b)
{
  W42MathNode *n = mk (name, NULL);

  w42_math_node_add (n, a);
  if (b != NULL)
    w42_math_node_add (n, b);
  return n;
}

/* The messages say what is wrong with the LaTeX; its commands -- \\begin,
 * \\left -- are LaTeX's and not translated. */
static void
fail (Tex *t, const char *message)
{
  if (!t->failed)
    g_set_error (t->error, G_MARKUP_ERROR, G_MARKUP_ERROR_PARSE, "%s", message);
  t->failed = TRUE;
}

static void
skip_space (Tex *t)
{
  while (t->i < t->len && g_ascii_isspace (t->s[t->i]))
    t->i++;
  /* A comment runs to the end of its line. */
  if (t->i < t->len && t->s[t->i] == '%')
    {
      while (t->i < t->len && t->s[t->i] != '\n')
        t->i++;
      skip_space (t);
    }
}

/* After a backslash: a command's name, letters, or one other character. */
static char *
read_name (Tex *t)
{
  gsize from = t->i;

  if (t->i >= t->len)
    return g_strdup ("");
  if (g_ascii_isalpha (t->s[t->i]))
    {
      while (t->i < t->len && g_ascii_isalpha (t->s[t->i]))
        t->i++;
      return g_strndup (t->s + from, t->i - from);
    }
  t->i += g_utf8_skip[(guchar) t->s[t->i]];
  return g_strndup (t->s + from, t->i - from);
}

/* The text of a {...} argument as it stands, braces balanced; NULL when
 * there is no brace. */
static char *
read_raw_group (Tex *t)
{
  gsize from;
  int depth = 1;

  skip_space (t);
  if (t->i >= t->len || t->s[t->i] != '{')
    return NULL;
  from = ++t->i;
  while (t->i < t->len)
    {
      char c = t->s[t->i];

      if (c == '\\' && t->i + 1 < t->len)
        {
          t->i += 2;
          continue;
        }
      if (c == '{')
        depth++;
      else if (c == '}' && --depth == 0)
        {
          char *raw = g_strndup (t->s + from, t->i - from);

          t->i++;
          return raw;
        }
      t->i++;
    }
  fail (t, _("A { has no } to close it."));
  return g_strndup (t->s + from, t->i - from);
}

/* An optional [...] argument, or NULL. */
static char *
read_optional (Tex *t)
{
  gsize from;
  int depth = 0;

  skip_space (t);
  if (t->i >= t->len || t->s[t->i] != '[')
    return NULL;
  from = ++t->i;
  while (t->i < t->len)
    {
      char c = t->s[t->i];

      if (c == '{') depth++;
      else if (c == '}') depth--;
      else if (c == ']' && depth == 0)
        {
          char *raw = g_strndup (t->s + from, t->i - from);

          t->i++;
          return raw;
        }
      t->i++;
    }
  fail (t, _("A [ has no ] to close it."));
  return NULL;
}

static GPtrArray *parse_list (Tex *t, Stop *stop);
static W42MathNode *parse_arg (Tex *t);

/* A row of what was parsed, or the one thing when it is one. */
static W42MathNode *
row_of (GPtrArray *nodes)
{
  W42MathNode *row;

  if (nodes->len == 1)
    {
      row = g_ptr_array_steal_index (nodes, 0);
      g_ptr_array_unref (nodes);
      return row;
    }
  row = mk ("mrow", NULL);
  for (guint i = 0; i < nodes->len; i++)
    w42_math_node_add (row, g_ptr_array_index (nodes, i));
  g_ptr_array_set_free_func (nodes, NULL);
  g_ptr_array_unref (nodes);
  return row;
}

static W42MathNode *
parse_sub (Tex *t, const char *tex)
{
  Tex sub = { tex, 0, strlen (tex), t->error, t->failed, NULL };
  Stop stop;
  GPtrArray *nodes = parse_list (&sub, &stop);

  if (stop != STOP_END)
    fail (&sub, _("The braces do not balance."));
  t->failed = sub.failed;
  g_free (sub.end_name);
  return row_of (nodes);
}

static W42MathNode *
mo (const char *text, gboolean stretchy)
{
  W42MathNode *n = mk ("mo", text);

  if (!stretchy)
    w42_math_node_set_attr (n, "stretchy", "false");
  return n;
}

/* Sets mathvariant on every letter and number in `n`. */
static void
set_variant (W42MathNode *n, const char *variant)
{
  if (g_str_equal (n->name, "mi") || g_str_equal (n->name, "mn") ||
      (g_str_equal (n->name, "mtext") && !g_str_equal (variant, "normal")))
    w42_math_node_set_attr (n, "mathvariant", variant);
  for (guint i = 0; i < n->children->len; i++)
    set_variant (g_ptr_array_index (n->children, i), variant);
}

/* A delimiter after \left, \right, \big and the like. */
static char *
read_delim (Tex *t)
{
  skip_space (t);
  if (t->i >= t->len)
    return g_strdup ("");
  if (t->s[t->i] == '\\')
    {
      char *name;

      t->i++;
      name = read_name (t);
      for (guint k = 0; k < G_N_ELEMENTS (SYMBOLS); k++)
        if (g_str_equal (SYMBOLS[k].cmd, name))
          {
            g_free (name);
            return g_strdup (SYMBOLS[k].utf8);
          }
      g_free (name);
      return g_strdup ("");
    }
  {
    gsize from = t->i;

    t->i += g_utf8_skip[(guchar) t->s[t->i]];
    if (t->s[from] == '.')
      return g_strdup ("");
    if (t->s[from] == '<')
      return g_strdup ("⟨");
    if (t->s[from] == '>')
      return g_strdup ("⟩");
    return g_strndup (t->s + from, t->i - from);
  }
}

/* The cells of an environment's rows, to \end{name}. */
static W42MathNode *
parse_table (Tex *t, const char *env)
{
  W42MathNode *table = mk ("mtable", NULL);
  W42MathNode *row = mk ("mtr", NULL);

  for (;;)
    {
      Stop stop;
      GPtrArray *cell = parse_list (t, &stop);
      W42MathNode *td = mk ("mtd", NULL);

      for (guint i = 0; i < cell->len; i++)
        w42_math_node_add (td, g_ptr_array_index (cell, i));
      g_ptr_array_set_free_func (cell, NULL);
      g_ptr_array_unref (cell);
      w42_math_node_add (row, td);
      if (stop == STOP_AMP)
        continue;
      if (stop == STOP_ROW)
        {
          w42_math_node_add (table, row);
          row = mk ("mtr", NULL);
          continue;
        }
      if (stop == STOP_ENV)
        {
          if (t->end_name == NULL || !g_str_equal (t->end_name, env))
            fail (t, _("\\begin and \\end name different environments."));
        }
      else
        fail (t, _("An environment has no \\end."));
      break;
    }
  /* A last row with nothing in it is the \\ at the end of the one before. */
  if (!(row->children->len == 1 &&
        ((W42MathNode *) g_ptr_array_index (row->children, 0))->children->len == 0 &&
        table->children->len > 0))
    w42_math_node_add (table, row);
  else
    w42_math_node_free (row);
  return table;
}

/* In amsmath's aligned columns a cell after an & begins with its
 * relation, which TeX spaces as though something stood before it: an
 * empty <mi> there, as MathJax puts, keeps the space. */
static void
space_relations (W42MathNode *table)
{
  for (guint r = 0; r < table->children->len; r++)
    {
      W42MathNode *row = g_ptr_array_index (table->children, r);

      for (guint c = 1; c < row->children->len; c += 2)
        {
          W42MathNode *td = g_ptr_array_index (row->children, c);
          W42MathNode *first = td->children->len > 0 ? g_ptr_array_index (td->children, 0) : NULL;

          if (first != NULL && g_str_equal (first->name, "mo"))
            g_ptr_array_insert (td->children, 0, mk ("mi", NULL));
        }
    }
}

static W42MathNode *
fenced (W42MathNode *inner, const char *open, const char *close)
{
  W42MathNode *row = mk ("mrow", NULL);

  if (open != NULL && *open != '\0')
    w42_math_node_add (row, mo (open, TRUE));
  w42_math_node_add (row, inner);
  if (close != NULL && *close != '\0')
    w42_math_node_add (row, mo (close, TRUE));
  return row;
}

static W42MathNode *
parse_env (Tex *t)
{
  char *env = read_raw_group (t);
  W42MathNode *table, *result;
  const char *open = NULL, *close = NULL;
  char *spec = NULL;

  if (env == NULL)
    {
      fail (t, _("\\begin names no environment."));
      return mk ("mrow", NULL);
    }
  if (g_str_equal (env, "array"))
    spec = read_raw_group (t);
  table = parse_table (t, env);

  if (g_str_equal (env, "pmatrix")) { open = "("; close = ")"; }
  else if (g_str_equal (env, "bmatrix")) { open = "["; close = "]"; }
  else if (g_str_equal (env, "Bmatrix")) { open = "{"; close = "}"; }
  else if (g_str_equal (env, "vmatrix")) { open = "|"; close = "|"; }
  else if (g_str_equal (env, "Vmatrix")) { open = "‖"; close = "‖"; }

  if (g_str_equal (env, "cases") || g_str_equal (env, "dcases"))
    {
      w42_math_node_set_attr (table, "columnalign", "left left");
      w42_math_node_set_attr (table, "columnspacing", "1em");
      open = "{";
      close = "";
    }
  else if (g_str_has_prefix (env, "align") || g_str_equal (env, "aligned") ||
           g_str_equal (env, "split") || g_str_has_prefix (env, "eqnarray"))
    {
      w42_math_node_set_attr (table, "columnalign", "right left right left right left");
      w42_math_node_set_attr (table, "columnspacing", "0em 2em 0em 2em 0em");
      w42_math_node_set_attr (table, "displaystyle", "true");
      space_relations (table);
    }
  else if (g_str_has_prefix (env, "gather") || g_str_equal (env, "gathered") ||
           g_str_has_prefix (env, "multline") || g_str_has_prefix (env, "equation"))
    w42_math_node_set_attr (table, "displaystyle", "true");
  else if (spec != NULL)
    {
      GString *aligns = g_string_new (NULL);

      for (const char *p = spec; *p != '\0'; p++)
        if (*p == 'l' || *p == 'c' || *p == 'r')
          g_string_append_printf (aligns, "%s%s", aligns->len ? " " : "",
                                  *p == 'l' ? "left" : *p == 'r' ? "right" : "center");
      if (aligns->len > 0)
        w42_math_node_set_attr (table, "columnalign", aligns->str);
      g_string_free (aligns, TRUE);
    }
  else if (g_str_equal (env, "smallmatrix"))
    w42_math_node_set_attr (table, "scriptlevel", "1");

  result = open != NULL || close != NULL ? fenced (table, open, close) : table;
  g_free (env);
  g_free (spec);
  return result;
}

/* Scripts after an atom: _, ^ and primes, in any order. */
static W42MathNode *
parse_scripts (Tex *t, W42MathNode *base, gboolean limits_under)
{
  W42MathNode *sub = NULL, *sup = NULL;
  GString *primes = g_string_new (NULL);

  for (;;)
    {
      skip_space (t);
      if (t->i >= t->len)
        break;
      if (t->s[t->i] == '^' && sup == NULL)
        {
          t->i++;
          sup = parse_arg (t);
        }
      else if (t->s[t->i] == '_' && sub == NULL)
        {
          t->i++;
          sub = parse_arg (t);
        }
      else if (t->s[t->i] == '\'')
        {
          t->i++;
          g_string_append (primes, "′");
        }
      else
        break;
    }
  if (primes->len > 0)
    {
      W42MathNode *p = mk ("mo", primes->str);

      if (sup != NULL)
        {
          W42MathNode *row = mk2 ("mrow", p, sup);

          sup = row;
        }
      else
        sup = p;
    }
  g_string_free (primes, TRUE);

  if (sub == NULL && sup == NULL)
    return base;
  if (limits_under)
    {
      if (sub != NULL && sup != NULL)
        {
          W42MathNode *n = mk2 ("munderover", base, sub);

          w42_math_node_add (n, sup);
          return n;
        }
      return mk2 (sub != NULL ? "munder" : "mover", base, sub != NULL ? sub : sup);
    }
  if (sub != NULL && sup != NULL)
    {
      W42MathNode *n = mk2 ("msubsup", base, sub);

      w42_math_node_add (n, sup);
      return n;
    }
  return mk2 (sub != NULL ? "msub" : "msup", base, sub != NULL ? sub : sup);
}

/* \limits or \nolimits after a large operator. */
static int
read_limits (Tex *t)
{
  gsize at;

  skip_space (t);
  at = t->i;
  if (t->i + 1 < t->len && t->s[t->i] == '\\')
    {
      char *name;

      t->i++;
      name = read_name (t);
      if (g_str_equal (name, "limits"))
        {
          g_free (name);
          return 1;
        }
      if (g_str_equal (name, "nolimits"))
        {
          g_free (name);
          return -1;
        }
      g_free (name);
    }
  t->i = at;
  return 0;
}

/* One command, whose name has been read: the nodes it makes go into
 * `out`.  Returns FALSE when it ends the list -- \right, \end. */
static gboolean
command (Tex *t, const char *name, GPtrArray *out, Stop *stop)
{
  W42MathNode *n = NULL;

  if (g_str_equal (name, "right"))
    {
      *stop = STOP_RIGHT;
      return FALSE;
    }
  if (g_str_equal (name, "end"))
    {
      g_free (t->end_name);
      t->end_name = read_raw_group (t);
      *stop = STOP_ENV;
      return FALSE;
    }
  if (g_str_equal (name, "\\") || g_str_equal (name, "cr") || g_str_equal (name, "newline"))
    {
      read_optional (t);
      *stop = STOP_ROW;
      return FALSE;
    }

  for (guint k = 0; k < G_N_ELEMENTS (SYMBOLS); k++)
    if (g_str_equal (SYMBOLS[k].cmd, name))
      {
        switch (SYMBOLS[k].kind)
          {
          case K_MI:
            n = mk ("mi", SYMBOLS[k].utf8);
            break;
          case K_MO:
            n = mk ("mo", SYMBOLS[k].utf8);
            break;
          case K_FENCE:
            n = mo (SYMBOLS[k].utf8, FALSE);
            break;
          case K_BIG:
          case K_BIGLIM:
            {
              int limits = read_limits (t);
              gboolean under = limits > 0 || (limits == 0 && SYMBOLS[k].kind == K_BIGLIM);

              n = parse_scripts (t, mk ("mo", SYMBOLS[k].utf8), under);
            }
            break;
          case K_FUNC:
            n = parse_scripts (t, mk ("mi", SYMBOLS[k].utf8), FALSE);
            g_ptr_array_add (out, n);
            n = mk ("mo", "\342\201\241");
            break;
          case K_FUNCLIM:
            {
              W42MathNode *op = mk ("mo", SYMBOLS[k].utf8);

              w42_math_node_set_attr (op, "movablelimits", "true");
              n = parse_scripts (t, op, read_limits (t) >= 0);
              g_ptr_array_add (out, n);
              n = mk ("mo", "\342\201\241");
            }
            break;
          }
        if (SYMBOLS[k].kind == K_MI || SYMBOLS[k].kind == K_MO || SYMBOLS[k].kind == K_FENCE)
          n = parse_scripts (t, n, FALSE);
        g_ptr_array_add (out, n);
        return TRUE;
      }

  for (guint k = 0; k < G_N_ELEMENTS (SPACES); k++)
    if (g_str_equal (SPACES[k].cmd, name))
      {
        n = mk ("mspace", NULL);
        w42_math_node_set_attr (n, "width", SPACES[k].em);
        g_ptr_array_add (out, n);
        return TRUE;
      }

  for (guint k = 0; k < G_N_ELEMENTS (ACCENTS); k++)
    if (g_str_equal (ACCENTS[k].cmd, name))
      {
        W42MathNode *base = parse_arg (t);
        W42MathNode *mark = mk ("mo", ACCENTS[k].utf8);

        n = mk2 (ACCENTS[k].under ? "munder" : "mover", base, mark);
        w42_math_node_set_attr (n, ACCENTS[k].under ? "accentunder" : "accent", "true");
        g_ptr_array_add (out, parse_scripts (t, n, FALSE));
        return TRUE;
      }

  for (guint k = 0; k < G_N_ELEMENTS (ALPHABETS); k++)
    if (g_str_equal (ALPHABETS[k].cmd, name))
      {
        n = parse_arg (t);
        set_variant (n, ALPHABETS[k].variant);
        g_ptr_array_add (out, parse_scripts (t, n, FALSE));
        return TRUE;
      }

  for (guint k = 0; k < G_N_ELEMENTS (BIGS); k++)
    if (g_str_equal (BIGS[k].cmd, name))
      {
        char *d = read_delim (t);

        n = mk ("mo", d);
        w42_math_node_set_attr (n, "minsize", BIGS[k].size);
        w42_math_node_set_attr (n, "maxsize", BIGS[k].size);
        g_free (d);
        g_ptr_array_add (out, n);
        return TRUE;
      }

  if (g_str_equal (name, "frac") || g_str_equal (name, "dfrac") ||
      g_str_equal (name, "tfrac") || g_str_equal (name, "cfrac"))
    {
      W42MathNode *a = parse_arg (t), *b = parse_arg (t);

      n = mk2 ("mfrac", a, b);
      if (name[0] == 'd' || name[0] == 'c' || name[0] == 't')
        {
          W42MathNode *style = mk ("mstyle", NULL);

          w42_math_node_set_attr (style, "displaystyle", name[0] == 't' ? "false" : "true");
          w42_math_node_add (style, n);
          n = style;
        }
    }
  else if (g_str_equal (name, "binom") || g_str_equal (name, "dbinom") ||
           g_str_equal (name, "tbinom") || g_str_equal (name, "choose"))
    {
      W42MathNode *a = parse_arg (t), *b = parse_arg (t);
      W42MathNode *frac = mk2 ("mfrac", a, b);

      w42_math_node_set_attr (frac, "linethickness", "0");
      n = fenced (frac, "(", ")");
      if (name[0] == 'd' || name[0] == 't')
        {
          W42MathNode *style = mk ("mstyle", NULL);

          w42_math_node_set_attr (style, "displaystyle", name[0] == 'd' ? "true" : "false");
          w42_math_node_add (style, n);
          n = style;
        }
    }
  else if (g_str_equal (name, "sqrt"))
    {
      char *index = read_optional (t);
      W42MathNode *body = parse_arg (t);

      if (index != NULL)
        n = mk2 ("mroot", body, parse_sub (t, index));
      else
        n = mk2 ("msqrt", body, NULL);
      g_free (index);
    }
  else if (g_str_equal (name, "left"))
    {
      char *open = read_delim (t);
      Stop inner;
      GPtrArray *nodes = parse_list (t, &inner);
      char *close = NULL;

      if (inner == STOP_RIGHT)
        close = read_delim (t);
      else
        fail (t, _("\\left has no \\right."));
      n = mk ("mrow", NULL);
      if (*open != '\0')
        w42_math_node_add (n, mo (open, TRUE));
      for (guint i = 0; i < nodes->len; i++)
        w42_math_node_add (n, g_ptr_array_index (nodes, i));
      g_ptr_array_set_free_func (nodes, NULL);
      g_ptr_array_unref (nodes);
      if (close != NULL && *close != '\0')
        w42_math_node_add (n, mo (close, TRUE));
      g_free (open);
      g_free (close);
    }
  else if (g_str_equal (name, "middle"))
    {
      char *d = read_delim (t);

      n = mo (d, TRUE);
      g_free (d);
    }
  else if (g_str_equal (name, "begin"))
    n = parse_env (t);
  else if (g_str_equal (name, "text") || g_str_equal (name, "textrm") ||
           g_str_equal (name, "textup") || g_str_equal (name, "mbox") ||
           g_str_equal (name, "textit") || g_str_equal (name, "textbf") ||
           g_str_equal (name, "textsf") || g_str_equal (name, "texttt") ||
           g_str_equal (name, "textnormal") || g_str_equal (name, "hbox"))
    {
      char *raw = read_raw_group (t);
      GString *text = g_string_new (NULL);

      /* A space or a brace escaped is itself; a nested group only its text. */
      for (const char *p = raw != NULL ? raw : ""; *p != '\0'; p++)
        {
          if (*p == '\\' && p[1] != '\0' && strchr (" {}$%&#_~", p[1]) != NULL)
            g_string_append_c (text, *++p);
          else if (*p != '{' && *p != '}')
            g_string_append_c (text, *p);
        }
      n = mk ("mtext", text->str);
      if (g_str_equal (name, "textit"))
        w42_math_node_set_attr (n, "mathvariant", "italic");
      else if (g_str_equal (name, "textbf"))
        w42_math_node_set_attr (n, "mathvariant", "bold");
      else if (g_str_equal (name, "textsf"))
        w42_math_node_set_attr (n, "mathvariant", "sans-serif");
      else if (g_str_equal (name, "texttt"))
        w42_math_node_set_attr (n, "mathvariant", "monospace");
      g_string_free (text, TRUE);
      g_free (raw);
    }
  else if (g_str_equal (name, "operatorname") || g_str_equal (name, "operatorname*"))
    {
      char *raw = read_raw_group (t);

      n = parse_scripts (t, mk ("mi", raw != NULL ? raw : ""), FALSE);
      g_ptr_array_add (out, n);
      n = mk ("mo", "\342\201\241");
      g_free (raw);
    }
  else if (g_str_equal (name, "overbrace") || g_str_equal (name, "underbrace"))
    {
      gboolean over = name[0] == 'o';
      W42MathNode *base = parse_arg (t);
      W42MathNode *brace = mk2 (over ? "mover" : "munder", base, mk ("mo", over ? "⏞" : "⏟"));

      skip_space (t);
      if (t->i < t->len && t->s[t->i] == (over ? '^' : '_'))
        {
          t->i++;
          n = mk2 (over ? "mover" : "munder", brace, parse_arg (t));
        }
      else
        n = brace;
    }
  else if (g_str_equal (name, "overset") || g_str_equal (name, "underset") ||
           g_str_equal (name, "stackrel"))
    {
      W42MathNode *mark = parse_arg (t), *base = parse_arg (t);

      n = mk2 (name[0] == 'u' ? "munder" : "mover", base, mark);
    }
  else if (g_str_equal (name, "not"))
    {
      static const struct { const char *from, *to; } NOT[] = {
        { "=", "≠" }, { "∈", "∉" }, { "<", "≮" }, { ">", "≯" }, { "≡", "≢" },
        { "⊂", "⊄" }, { "⊃", "⊅" }, { "∼", "≁" }, { "≤", "≰" }, { "≥", "≱" },
        { "⊆", "⊈" }, { "⊇", "⊉" }, { "∣", "∤" }, { "∥", "∦" }, { "≈", "≉" },
      };
      Stop dummy;
      GPtrArray *next = g_ptr_array_new_with_free_func ((GDestroyNotify) w42_math_node_free);

      skip_space (t);
      if (t->i < t->len && t->s[t->i] == '\\')
        {
          char *nm;

          t->i++;
          nm = read_name (t);
          command (t, nm, next, &dummy);
          g_free (nm);
        }
      else if (t->i < t->len)
        {
          gsize from = t->i;
          char *ch;

          t->i += g_utf8_skip[(guchar) t->s[t->i]];
          ch = g_strndup (t->s + from, t->i - from);
          g_ptr_array_add (next, mk ("mo", ch));
          g_free (ch);
        }
      if (next->len == 1)
        {
          W42MathNode *m = g_ptr_array_index (next, 0);

          if (m->text != NULL)
            {
              const char *to = NULL;

              for (guint k = 0; k < G_N_ELEMENTS (NOT); k++)
                if (g_str_equal (NOT[k].from, m->text))
                  to = NOT[k].to;
              if (to != NULL)
                n = mk ("mo", to);
              else
                {
                  char *slashed = g_strconcat (m->text, "\314\270", NULL);

                  n = mk ("mo", slashed);
                  g_free (slashed);
                }
            }
        }
      g_ptr_array_unref (next);
      if (n == NULL)
        n = mk ("mo", "\314\270");
    }
  else if (g_str_equal (name, "color") || g_str_equal (name, "textcolor"))
    {
      char *colour = read_raw_group (t);

      if (g_str_equal (name, "textcolor"))
        n = mk2 ("mstyle", parse_arg (t), NULL);
      else
        {
          /* \color colours the rest of the group. */
          GPtrArray *rest = parse_list (t, stop);

          n = mk2 ("mstyle", row_of (rest), NULL);
          w42_math_node_set_attr (n, "mathcolor", colour != NULL ? colour : "black");
          g_free (colour);
          g_ptr_array_add (out, n);
          return FALSE;
        }
      w42_math_node_set_attr (n, "mathcolor", colour != NULL ? colour : "black");
      g_free (colour);
    }
  else if (g_str_equal (name, "displaystyle") || g_str_equal (name, "textstyle") ||
           g_str_equal (name, "scriptstyle") || g_str_equal (name, "scriptscriptstyle"))
    {
      GPtrArray *rest = parse_list (t, stop);

      n = mk2 ("mstyle", row_of (rest), NULL);
      w42_math_node_set_attr (n, "displaystyle", name[0] == 'd' ? "true" : "false");
      if (name[0] == 's')
        w42_math_node_set_attr (n, "scriptlevel", name[6] == 's' ? "2" : "1");
      else
        w42_math_node_set_attr (n, "scriptlevel", "0");
      g_ptr_array_add (out, n);
      return FALSE;
    }
  else if (g_str_equal (name, "boxed") || g_str_equal (name, "fbox"))
    {
      n = mk2 ("menclose", parse_arg (t), NULL);
      w42_math_node_set_attr (n, "notation", "box");
    }
  else if (g_str_equal (name, "phantom"))
    n = mk2 ("mphantom", parse_arg (t), NULL);
  else if (g_str_equal (name, "cancel"))
    {
      n = mk2 ("menclose", parse_arg (t), NULL);
      w42_math_node_set_attr (n, "notation", "updiagonalstrike");
    }
  else if (g_str_equal (name, "pmod") || g_str_equal (name, "bmod") || g_str_equal (name, "mod"))
    {
      W42MathNode *space = mk ("mspace", NULL);

      w42_math_node_set_attr (space, "width", name[0] == 'p' ? "1em" : "0.2778em");
      g_ptr_array_add (out, space);
      if (name[0] == 'p')
        {
          W42MathNode *arg = parse_arg (t);
          W42MathNode *row = mk ("mrow", NULL);

          w42_math_node_add (row, mo ("(", FALSE));
          w42_math_node_add (row, mk ("mi", "mod"));
          space = mk ("mspace", NULL);
          w42_math_node_set_attr (space, "width", "0.3333em");
          w42_math_node_add (row, space);
          w42_math_node_add (row, arg);
          w42_math_node_add (row, mo (")", FALSE));
          n = row;
        }
      else
        n = mk ("mo", "mod");
    }
  else if (g_str_equal (name, "tag") || g_str_equal (name, "label") ||
           g_str_equal (name, "notag") || g_str_equal (name, "nonumber") ||
           g_str_equal (name, "limits") || g_str_equal (name, "nolimits") ||
           g_str_equal (name, "tag*"))
    {
      /* A number or a label for the equation, which the document gives it
       * its own way: nothing to show. */
      if (g_str_equal (name, "tag") || g_str_equal (name, "label"))
        g_free (read_raw_group (t));
      return TRUE;
    }
  else if (strchr ("{}$%&#_", name[0]) != NULL && name[1] == '\0')
    n = mk (name[0] == '{' || name[0] == '}' ? "mo" : "mi", name);
  else
    {
      /* Not known: shown as typed, in red, as a note to fix it. */
      char *shown = g_strconcat ("\\", name, NULL);

      n = mk2 ("merror", mk ("mtext", shown), NULL);
      g_free (shown);
    }
  g_ptr_array_add (out, parse_scripts (t, n, FALSE));
  return TRUE;
}

/* What is left of a character not a command. */
static W42MathNode *
char_atom (Tex *t)
{
  gunichar c = g_utf8_get_char (t->s + t->i);
  gsize from = t->i;
  char *text;
  W42MathNode *n;

  if (g_ascii_isdigit (c) || (c == '.' && t->i + 1 < t->len && g_ascii_isdigit (t->s[t->i + 1])))
    {
      while (t->i < t->len && (g_ascii_isdigit (t->s[t->i]) ||
                               (t->s[t->i] == '.' && t->i + 1 < t->len &&
                                g_ascii_isdigit (t->s[t->i + 1]))))
        t->i++;
      text = g_strndup (t->s + from, t->i - from);
      n = mk ("mn", text);
      g_free (text);
      return n;
    }
  t->i += g_utf8_skip[(guchar) t->s[t->i]];
  text = g_strndup (t->s + from, t->i - from);
  if (c == '~')
    {
      n = mk ("mspace", NULL);
      w42_math_node_set_attr (n, "width", "0.3333em");
    }
  else if (g_unichar_isalpha (c))
    n = mk ("mi", text);
  else if (g_unichar_isdigit (c))
    n = mk ("mn", text);
  else if (strchr ("()[]|", (int) c) != NULL && c < 0x80)
    n = mo (text, FALSE);
  else if (c == '-')
    n = mk ("mo", "−");
  else if (c == '*')
    n = mk ("mo", "∗");
  else
    n = mk ("mo", text);
  g_free (text);
  return n;
}

/* A {group}, a command, or one character: what ^ and _ and \frac take. */
static W42MathNode *
parse_arg (Tex *t)
{
  skip_space (t);
  if (t->i >= t->len)
    {
      fail (t, _("Something is missing after a command, a ^ or a _."));
      return mk ("mrow", NULL);
    }
  if (t->s[t->i] == '{')
    {
      Stop stop;
      GPtrArray *nodes;

      t->i++;
      nodes = parse_list (t, &stop);
      if (stop != STOP_BRACE)
        fail (t, _("A { has no } to close it."));
      return row_of (nodes);
    }
  if (t->s[t->i] == '\\')
    {
      GPtrArray *out = g_ptr_array_new_with_free_func ((GDestroyNotify) w42_math_node_free);
      char *name;
      Stop stop = STOP_END;

      t->i++;
      name = read_name (t);
      command (t, name, out, &stop);
      g_free (name);
      if (out->len == 0)
        {
          g_ptr_array_unref (out);
          return mk ("mrow", NULL);
        }
      return row_of (out);
    }
  if (t->s[t->i] == '}' || t->s[t->i] == '^' || t->s[t->i] == '_' || t->s[t->i] == '&')
    {
      fail (t, _("Something is missing after a command, a ^ or a _."));
      return mk ("mrow", NULL);
    }
  return char_atom (t);
}

static GPtrArray *
parse_list (Tex *t, Stop *stop)
{
  GPtrArray *out = g_ptr_array_new_with_free_func ((GDestroyNotify) w42_math_node_free);

  *stop = STOP_END;
  for (;;)
    {
      char c;

      skip_space (t);
      if (t->i >= t->len)
        {
          *stop = STOP_END;
          return out;
        }
      c = t->s[t->i];
      if (c == '}')
        {
          t->i++;
          *stop = STOP_BRACE;
          return out;
        }
      if (c == '&')
        {
          t->i++;
          *stop = STOP_AMP;
          return out;
        }
      if (c == '{')
        {
          Stop inner;
          GPtrArray *nodes;

          t->i++;
          nodes = parse_list (t, &inner);
          if (inner != STOP_BRACE)
            {
              fail (t, _("A { has no } to close it."));
              g_ptr_array_add (out, row_of (nodes));
              *stop = inner;
              return out;
            }
          g_ptr_array_add (out, parse_scripts (t, row_of (nodes), FALSE));
          continue;
        }
      if (c == '\\')
        {
          char *name;
          gboolean go_on;

          t->i++;
          name = read_name (t);
          go_on = command (t, name, out, stop);
          g_free (name);
          if (!go_on)
            return out;
          continue;
        }
      if (c == '^' || c == '_')
        {
          /* A script with nothing before it goes on nothing. */
          g_ptr_array_add (out, parse_scripts (t, mk ("mrow", NULL), FALSE));
          continue;
        }
      g_ptr_array_add (out, parse_scripts (t, char_atom (t), FALSE));
    }
}

char *
w42_tex_to_mathml (const char *tex, gboolean display, GError **error)
{
  Tex t = { tex, 0, 0, error, FALSE, NULL };
  Stop stop;
  GPtrArray *nodes;
  W42MathNode *math, *sem, *body, *note;
  char *out;

  g_return_val_if_fail (tex != NULL, NULL);
  t.len = strlen (tex);
  nodes = parse_list (&t, &stop);

  /* Lines and columns at the top: as amsmath's aligned sets them. */
  if (stop == STOP_ROW || stop == STOP_AMP)
    {
      W42MathNode *table = mk ("mtable", NULL), *row = mk ("mtr", NULL);

      for (;;)
        {
          W42MathNode *td = mk ("mtd", NULL);

          for (guint i = 0; i < nodes->len; i++)
            w42_math_node_add (td, g_ptr_array_index (nodes, i));
          g_ptr_array_set_free_func (nodes, NULL);
          g_ptr_array_unref (nodes);
          w42_math_node_add (row, td);
          if (stop == STOP_ROW)
            {
              w42_math_node_add (table, row);
              row = mk ("mtr", NULL);
            }
          if (stop != STOP_ROW && stop != STOP_AMP)
            break;
          nodes = parse_list (&t, &stop);
        }
      /* A \\ at the very end ends the last line, not begins another. */
      if (row->children->len > 1 ||
          (row->children->len == 1 &&
           ((W42MathNode *) g_ptr_array_index (row->children, 0))->children->len > 0))
        w42_math_node_add (table, row);
      else
        w42_math_node_free (row);
      w42_math_node_set_attr (table, "columnalign", "right left right left");
      w42_math_node_set_attr (table, "columnspacing", "0em 2em 0em");
      w42_math_node_set_attr (table, "displaystyle", "true");
      space_relations (table);
      nodes = g_ptr_array_new_with_free_func ((GDestroyNotify) w42_math_node_free);
      g_ptr_array_add (nodes, table);
    }
  if (stop == STOP_BRACE)
    fail (&t, _("A } has no { before it."));
  else if (stop == STOP_RIGHT)
    fail (&t, _("\\right has no \\left."));
  else if (stop == STOP_ENV)
    fail (&t, _("\\end has no \\begin."));
  g_free (t.end_name);

  if (t.failed)
    {
      g_ptr_array_unref (nodes);
      return NULL;
    }

  body = mk ("mrow", NULL);
  for (guint i = 0; i < nodes->len; i++)
    w42_math_node_add (body, g_ptr_array_index (nodes, i));
  g_ptr_array_set_free_func (nodes, NULL);
  g_ptr_array_unref (nodes);

  math = mk ("math", NULL);
  if (display)
    w42_math_node_set_attr (math, "display", "block");
  sem = mk ("semantics", NULL);
  w42_math_node_add (sem, body);
  note = mk ("annotation", tex);
  w42_math_node_set_attr (note, "encoding", W42_TEX_ENCODING);
  w42_math_node_add (sem, note);
  w42_math_node_add (math, sem);
  out = w42_math_node_to_string (math);
  w42_math_node_free (math);
  return out;
}

/* ---------------------------------------------------------------------- */
/* MathML to LaTeX                                                         */
/* ---------------------------------------------------------------------- */

static void to_tex (GString *out, const W42MathNode *n);

/* A separator where a command's name would run into a letter after it. */
static void
tex_word (GString *out, const char *word)
{
  if (out->len > 0 && word[0] != '\\' && g_ascii_isalpha (word[0]))
    {
      /* After \alpha, a letter needs a space; after a letter it does not. */
      gssize k = (gssize) out->len - 1;

      while (k >= 0 && g_ascii_isalpha (out->str[k]))
        k--;
      if (k >= 0 && out->str[k] == '\\' && (gsize) k + 1 < out->len)
        g_string_append_c (out, ' ');
    }
  g_string_append (out, word);
}

/* A character as LaTeX spells it. */
static void
tex_char (GString *out, gunichar c)
{
  char utf8[8] = { 0 };

  g_unichar_to_utf8 (c, utf8);
  switch (c)
    {
    case 0x2061: case 0x2062: case 0x2063: case 0x2064:
      return;
    case 0x2212: tex_word (out, "-"); return;
    case '{': tex_word (out, "\\{"); return;
    case '}': tex_word (out, "\\}"); return;
    case '#': tex_word (out, "\\#"); return;
    case '$': tex_word (out, "\\$"); return;
    case '%': tex_word (out, "\\%"); return;
    case '&': tex_word (out, "\\&"); return;
    case '_': tex_word (out, "\\_"); return;
    case '\\': tex_word (out, "\\backslash "); return;
    case 0x2032: tex_word (out, "'"); return;
    case 0x2033: tex_word (out, "''"); return;
    case 0xA0: tex_word (out, "~"); return;
    case 0x2016: tex_word (out, "\\|"); return;
    default:
      break;
    }
  if (c < 0x80)
    {
      g_string_append (out, utf8);
      return;
    }
  for (guint k = 0; k < G_N_ELEMENTS (SYMBOLS); k++)
    if (SYMBOLS[k].kind != K_FUNC && SYMBOLS[k].kind != K_FUNCLIM &&
        g_str_equal (SYMBOLS[k].utf8, utf8))
      {
        g_string_append_c (out, '\\');
        g_string_append (out, SYMBOLS[k].cmd);
        g_string_append_c (out, ' ');
        return;
      }
  g_string_append (out, utf8);
}

static void
tex_text (GString *out, const char *s)
{
  for (const char *p = s; p != NULL && *p != '\0'; p = g_utf8_next_char (p))
    tex_char (out, g_utf8_get_char (p));
}

/* A script's argument: in braces unless it is one character. */
static void
tex_arg (GString *out, const W42MathNode *n)
{
  GString *inner = g_string_new (NULL);

  to_tex (inner, n);
  while (inner->len > 0 && inner->str[inner->len - 1] == ' ')
    g_string_truncate (inner, inner->len - 1);
  if (g_utf8_strlen (inner->str, -1) == 1 && inner->str[0] != '\\')
    g_string_append (out, inner->str);
  else
    g_string_append_printf (out, "{%s}", inner->str);
  g_string_free (inner, TRUE);
}

static const W42MathNode *
kid (const W42MathNode *n, guint i)
{
  return i < n->children->len ? g_ptr_array_index (n->children, i) : NULL;
}

static void
tex_kids (GString *out, const W42MathNode *n, guint from)
{
  for (guint i = from; i < n->children->len; i++)
    to_tex (out, g_ptr_array_index (n->children, i));
}

/* The accent command for a character over something, or NULL. */
static const char *
accent_cmd (const char *mark, gboolean under, gboolean wide)
{
  if (mark == NULL)
    return NULL;
  if (under)
    {
      if (g_str_equal (mark, "_") || g_str_equal (mark, "¯") || g_str_equal (mark, "‾"))
        return "underline";
      if (g_str_equal (mark, "⏟"))
        return "underbrace";
      return NULL;
    }
  if (g_str_equal (mark, "⏞"))
    return "overbrace";
  if (g_str_equal (mark, "ˆ"))
    return wide ? "widehat" : "hat";
  if (g_str_equal (mark, "˜"))
    return wide ? "widetilde" : "tilde";
  if (g_str_equal (mark, "¯") || g_str_equal (mark, "‾") || g_str_equal (mark, "―"))
    return wide ? "overline" : "bar";
  if (g_str_equal (mark, "→") || g_str_equal (mark, "⃗"))
    return wide ? "overrightarrow" : "vec";
  for (guint k = 0; k < G_N_ELEMENTS (ACCENTS); k++)
    if (!ACCENTS[k].under && g_str_equal (ACCENTS[k].utf8, mark))
      return wide && g_str_equal (ACCENTS[k].cmd, "hat") ? "widehat"
           : wide && g_str_equal (ACCENTS[k].cmd, "tilde") ? "widetilde" : ACCENTS[k].cmd;
  return NULL;
}

static gboolean
is_fence_mo (const W42MathNode *n)
{
  return n != NULL && g_str_equal (n->name, "mo") && n->text != NULL &&
         (strstr ("()[]{}|‖⟨⟩⌈⌉⌊⌋", n->text) != NULL) && *n->text != '\0' &&
         *g_utf8_next_char (n->text) == '\0';
}

static void
tex_delim (GString *out, const char *d)
{
  if (d == NULL || *d == '\0')
    g_string_append_c (out, '.');
  else if (g_str_equal (d, "{"))
    g_string_append (out, "\\{");
  else if (g_str_equal (d, "}"))
    g_string_append (out, "\\}");
  else
    tex_text (out, d);
}

static void
tex_table (GString *out, const W42MathNode *n, const char *env)
{
  g_string_append_printf (out, "\\begin{%s}", env);
  for (guint r = 0; r < n->children->len; r++)
    {
      const W42MathNode *row = kid (n, r);

      if (r > 0)
        g_string_append (out, " \\\\ ");
      for (guint c = g_str_equal (row->name, "mlabeledtr") ? 1 : 0; c < row->children->len; c++)
        {
          if (c > (g_str_equal (row->name, "mlabeledtr") ? 1u : 0u))
            g_string_append (out, " & ");
          to_tex (out, kid (row, c));
        }
    }
  g_string_append_printf (out, "\\end{%s}", env);
}

static void
to_tex (GString *out, const W42MathNode *n)
{
  const char *name = n->name;

  if (g_str_equal (name, "mi"))
    {
      const char *variant = w42_math_node_attr (n, "mathvariant");
      const char *text = n->text != NULL ? n->text : "";
      glong len = g_utf8_strlen (text, -1);

      for (guint k = 0; k < G_N_ELEMENTS (SYMBOLS); k++)
        if ((SYMBOLS[k].kind == K_FUNC || SYMBOLS[k].kind == K_FUNCLIM) &&
            g_str_equal (SYMBOLS[k].utf8, text))
          {
            g_string_append_printf (out, "\\%s ", SYMBOLS[k].cmd);
            return;
          }
      if (variant != NULL && !g_str_equal (variant, "italic"))
        {
          for (guint k = 0; k < G_N_ELEMENTS (ALPHABETS); k++)
            if (g_str_equal (ALPHABETS[k].variant, variant))
              {
                g_string_append_printf (out, "\\%s{", ALPHABETS[k].cmd);
                tex_text (out, text);
                g_string_append_c (out, '}');
                return;
              }
        }
      if (len > 1)
        {
          g_string_append (out, "\\mathrm{");
          tex_text (out, text);
          g_string_append_c (out, '}');
          return;
        }
      tex_text (out, text);
      return;
    }
  if (g_str_equal (name, "mn"))
    {
      tex_text (out, n->text);
      return;
    }
  if (g_str_equal (name, "mo"))
    {
      const char *text = n->text != NULL ? n->text : "";
      const char *minsize = w42_math_node_attr (n, "minsize");

      for (guint k = 0; k < G_N_ELEMENTS (SYMBOLS); k++)
        if (SYMBOLS[k].kind == K_FUNCLIM && g_str_equal (SYMBOLS[k].utf8, text))
          {
            g_string_append_printf (out, "\\%s ", SYMBOLS[k].cmd);
            return;
          }
      if (minsize != NULL)
        for (guint k = 0; k < G_N_ELEMENTS (BIGS); k += 4)
          if (g_str_equal (BIGS[k].size, minsize))
            {
              g_string_append_printf (out, "\\%s", BIGS[k].cmd);
              tex_delim (out, text);
              return;
            }
      if (g_str_equal (text, "{") || g_str_equal (text, "}"))
        {
          g_string_append_printf (out, "\\%s", text);
          return;
        }
      if (g_utf8_strlen (text, -1) > 1 && g_ascii_isalpha (text[0]))
        {
          g_string_append_printf (out, "\\operatorname{%s}", text);
          return;
        }
      tex_text (out, text);
      return;
    }
  if (g_str_equal (name, "mtext") || g_str_equal (name, "ms"))
    {
      g_string_append (out, "\\text{");
      for (const char *p = n->text != NULL ? n->text : ""; *p != '\0'; p++)
        {
          if (strchr ("{}$%&#_", *p) != NULL)
            g_string_append_c (out, '\\');
          if (*p == '\\')
            g_string_append (out, "\\textbackslash{}");
          else if (*p == '~' || *p == '^')
            g_string_append_printf (out, "\\%c{}", *p);
          else
            g_string_append_c (out, *p);
        }
      g_string_append_c (out, '}');
      return;
    }
  if (g_str_equal (name, "mspace"))
    {
      const char *w = w42_math_node_attr (n, "width");
      double em = w != NULL ? g_ascii_strtod (w, NULL) : 0.0;

      if (w == NULL) return;
      if (em < -0.1) g_string_append (out, "\\!");
      else if (em < 0.2) g_string_append (out, "\\,");
      else if (em < 0.25) g_string_append (out, "\\:");
      else if (em < 0.4) g_string_append (out, "\\;");
      else if (em < 1.5) g_string_append (out, "\\quad ");
      else g_string_append (out, "\\qquad ");
      return;
    }
  if (g_str_equal (name, "msup") || g_str_equal (name, "msub") || g_str_equal (name, "msubsup"))
    {
      const W42MathNode *base = kid (n, 0);

      if (base == NULL)
        return;
      if (g_str_equal (base->name, "mrow") && base->children->len != 1)
        {
          g_string_append_c (out, '{');
          to_tex (out, base);
          g_string_append_c (out, '}');
        }
      else
        to_tex (out, base);
      if (g_str_equal (name, "msup"))
        {
          const W42MathNode *s = kid (n, 1);

          /* A prime is a prime, not a superscript of one. */
          if (s != NULL && g_str_equal (s->name, "mo") && s->text != NULL &&
              (g_str_equal (s->text, "′") || g_str_equal (s->text, "″") || g_str_equal (s->text, "'")))
            {
              g_string_append (out, g_str_equal (s->text, "″") ? "''" : "'");
              return;
            }
          g_string_append_c (out, '^');
          if (s != NULL) tex_arg (out, s);
          return;
        }
      g_string_append_c (out, '_');
      if (kid (n, 1) != NULL) tex_arg (out, kid (n, 1));
      if (g_str_equal (name, "msubsup") && kid (n, 2) != NULL)
        {
          g_string_append_c (out, '^');
          tex_arg (out, kid (n, 2));
        }
      return;
    }
  if (g_str_equal (name, "munder") || g_str_equal (name, "mover") || g_str_equal (name, "munderover"))
    {
      const W42MathNode *base = kid (n, 0);
      gboolean has_under = !g_str_equal (name, "mover");
      const W42MathNode *under = has_under ? kid (n, 1) : NULL;
      const W42MathNode *over = g_str_equal (name, "munderover") ? kid (n, 2)
                              : g_str_equal (name, "mover") ? kid (n, 1) : NULL;
      gboolean big = base != NULL && g_str_equal (base->name, "mo");
      const char *cmd;

      if (base == NULL)
        return;
      if (!big && over != NULL && under == NULL && g_str_equal (over->name, "mo") &&
          (cmd = accent_cmd (over->text, FALSE,
                             !(g_str_equal (base->name, "mi") && g_utf8_strlen (base->text ? base->text : "", -1) == 1))) != NULL)
        {
          g_string_append_printf (out, "\\%s{", cmd);
          to_tex (out, base);
          g_string_append_c (out, '}');
          return;
        }
      if (!big && under != NULL && over == NULL && g_str_equal (under->name, "mo") &&
          (cmd = accent_cmd (under->text, TRUE, TRUE)) != NULL)
        {
          g_string_append_printf (out, "\\%s{", cmd);
          to_tex (out, base);
          g_string_append_c (out, '}');
          return;
        }
      if (big)
        {
          to_tex (out, base);
          if (w42_math_node_attr (base, "movablelimits") == NULL)
            {
              const char *t = base->text != NULL ? base->text : "";

              if (strstr ("∫∬∭∮∯", t) != NULL && *t != '\0')
                g_string_append (out, "\\limits");
            }
          if (under != NULL) { g_string_append_c (out, '_'); tex_arg (out, under); }
          if (over != NULL) { g_string_append_c (out, '^'); tex_arg (out, over); }
          return;
        }
      /* A brace with its label: \underbrace{x+y}_{n}. */
      if ((under != NULL) != (over != NULL) && g_str_equal (base->name, under != NULL ? "munder" : "mover") &&
          kid (base, 1) != NULL && g_str_equal (kid (base, 1)->name, "mo") && kid (base, 1)->text != NULL &&
          g_str_equal (kid (base, 1)->text, under != NULL ? "⏟" : "⏞"))
        {
          g_string_append_printf (out, "\\%s{", under != NULL ? "underbrace" : "overbrace");
          to_tex (out, kid (base, 0));
          g_string_append_printf (out, "}%c", under != NULL ? '_' : '^');
          tex_arg (out, under != NULL ? under : over);
          return;
        }
      /* Something set over or under something else. */
      if (under != NULL && over != NULL)
        {
          g_string_append (out, "\\underset{");
          to_tex (out, under);
          g_string_append (out, "}{\\overset{");
          to_tex (out, over);
          g_string_append (out, "}{");
          to_tex (out, base);
          g_string_append (out, "}}");
          return;
        }
      g_string_append_printf (out, "\\%s{", under != NULL ? "underset" : "overset");
      to_tex (out, under != NULL ? under : over);
      g_string_append (out, "}{");
      to_tex (out, base);
      g_string_append_c (out, '}');
      return;
    }
  if (g_str_equal (name, "mfrac"))
    {
      const char *lt = w42_math_node_attr (n, "linethickness");

      if (lt != NULL && (g_str_equal (lt, "0") || g_str_equal (lt, "0px") || g_str_equal (lt, "0em")))
        g_string_append (out, "\\genfrac{}{}{0pt}{}{");
      else
        g_string_append (out, "\\frac{");
      if (kid (n, 0)) to_tex (out, kid (n, 0));
      g_string_append (out, "}{");
      if (kid (n, 1)) to_tex (out, kid (n, 1));
      g_string_append_c (out, '}');
      return;
    }
  if (g_str_equal (name, "msqrt"))
    {
      g_string_append (out, "\\sqrt{");
      tex_kids (out, n, 0);
      g_string_append_c (out, '}');
      return;
    }
  if (g_str_equal (name, "mroot"))
    {
      g_string_append (out, "\\sqrt[");
      if (kid (n, 1)) to_tex (out, kid (n, 1));
      g_string_append (out, "]{");
      if (kid (n, 0)) to_tex (out, kid (n, 0));
      g_string_append_c (out, '}');
      return;
    }
  if (g_str_equal (name, "mtable"))
    {
      const char *align = w42_math_node_attr (n, "columnalign");

      tex_table (out, n, align != NULL && g_str_has_prefix (align, "right left") ? "aligned" : "matrix");
      return;
    }
  if (g_str_equal (name, "mfenced"))
    {
      const char *open = w42_math_node_attr (n, "open"), *close = w42_math_node_attr (n, "close");

      g_string_append (out, "\\left");
      tex_delim (out, open != NULL ? open : "(");
      for (guint i = 0; i < n->children->len; i++)
        {
          if (i > 0) g_string_append_c (out, ',');
          to_tex (out, kid (n, i));
        }
      g_string_append (out, "\\right");
      tex_delim (out, close != NULL ? close : ")");
      return;
    }
  if (g_str_equal (name, "menclose"))
    {
      const char *notation = w42_math_node_attr (n, "notation");

      if (notation != NULL && strstr (notation, "radical") != NULL)
        g_string_append (out, "\\sqrt{");
      else if (notation != NULL && strstr (notation, "strike") != NULL)
        g_string_append (out, "\\cancel{");
      else
        g_string_append (out, "\\boxed{");
      tex_kids (out, n, 0);
      g_string_append_c (out, '}');
      return;
    }
  if (g_str_equal (name, "mphantom"))
    {
      g_string_append (out, "\\phantom{");
      tex_kids (out, n, 0);
      g_string_append_c (out, '}');
      return;
    }
  if (g_str_equal (name, "mstyle"))
    {
      const char *colour = w42_math_node_attr (n, "mathcolor");
      const char *display = w42_math_node_attr (n, "displaystyle");

      if (colour != NULL)
        {
          g_string_append_printf (out, "\\textcolor{%s}{", colour);
          tex_kids (out, n, 0);
          g_string_append_c (out, '}');
          return;
        }
      if (display != NULL)
        g_string_append_printf (out, "{\\%s ", g_str_equal (display, "true") ? "displaystyle" : "textstyle");
      tex_kids (out, n, 0);
      if (display != NULL)
        g_string_append_c (out, '}');
      return;
    }
  if (g_str_equal (name, "mmultiscripts"))
    {
      gboolean pre = FALSE;
      guint split = n->children->len;

      for (guint i = 1; i < n->children->len; i++)
        if (g_str_equal (kid (n, i)->name, "mprescripts"))
          {
            split = i;
            pre = TRUE;
          }
      if (pre)
        {
          g_string_append (out, "{}");
          for (guint i = split + 1; i < n->children->len; i += 2)
            {
              if (!g_str_equal (kid (n, i)->name, "none"))
                { g_string_append_c (out, '_'); tex_arg (out, kid (n, i)); }
              if (kid (n, i + 1) != NULL && !g_str_equal (kid (n, i + 1)->name, "none"))
                { g_string_append_c (out, '^'); tex_arg (out, kid (n, i + 1)); }
            }
        }
      if (kid (n, 0)) tex_arg (out, kid (n, 0));
      for (guint i = 1; i < split; i += 2)
        {
          if (!g_str_equal (kid (n, i)->name, "none"))
            { g_string_append_c (out, '_'); tex_arg (out, kid (n, i)); }
          if (i + 1 < split && !g_str_equal (kid (n, i + 1)->name, "none"))
            { g_string_append_c (out, '^'); tex_arg (out, kid (n, i + 1)); }
        }
      return;
    }
  if (g_str_equal (name, "semantics"))
    {
      if (kid (n, 0)) to_tex (out, kid (n, 0));
      return;
    }
  if (g_str_equal (name, "annotation") || g_str_equal (name, "annotation-xml") ||
      g_str_equal (name, "none") || g_str_equal (name, "mprescripts"))
    return;
  if (g_str_equal (name, "mrow") && n->children->len >= 2 &&
      is_fence_mo (kid (n, 0)) && is_fence_mo (kid (n, n->children->len - 1)) &&
      g_strcmp0 (w42_math_node_attr (kid (n, 0), "stretchy"), "false") != 0)
    {
      /* A bracketed row: \left and \right, which grow as MathML's do. */
      const W42MathNode *table = n->children->len == 3 ? kid (n, 1) : NULL;

      if (table != NULL && g_str_equal (table->name, "mtable"))
        {
          const char *o = kid (n, 0)->text, *c = kid (n, 2)->text;
          const char *env = g_str_equal (o, "(") && g_str_equal (c, ")") ? "pmatrix"
                          : g_str_equal (o, "[") && g_str_equal (c, "]") ? "bmatrix"
                          : g_str_equal (o, "{") && g_str_equal (c, "}") ? "Bmatrix"
                          : g_str_equal (o, "|") && g_str_equal (c, "|") ? "vmatrix"
                          : g_str_equal (o, "‖") && g_str_equal (c, "‖") ? "Vmatrix" : NULL;

          if (env != NULL)
            {
              tex_table (out, table, env);
              return;
            }
        }
      g_string_append (out, "\\left");
      tex_delim (out, kid (n, 0)->text);
      for (guint i = 1; i + 1 < n->children->len; i++)
        to_tex (out, kid (n, i));
      g_string_append (out, "\\right");
      tex_delim (out, kid (n, n->children->len - 1)->text);
      return;
    }
  if (g_str_equal (name, "mrow") && n->children->len == 2 && is_fence_mo (kid (n, 0)) &&
      g_str_equal (kid (n, 0)->text, "{") && g_str_equal (kid (n, 1)->name, "mtable"))
    {
      tex_table (out, kid (n, 1), "cases");
      return;
    }
  tex_kids (out, n, 0);
}

char *
w42_mathml_to_tex (const W42MathNode *root)
{
  const char *note;
  GString *out;

  g_return_val_if_fail (root != NULL, NULL);
  note = w42_math_annotation (root, W42_TEX_ENCODING);
  if (note == NULL)
    note = w42_math_annotation (root, "TeX");
  if (note == NULL)
    note = w42_math_annotation (root, "LaTeX");
  if (note != NULL)
    return g_strstrip (g_strdup (note));
  out = g_string_new (NULL);
  to_tex (out, root);
  while (out->len > 0 && out->str[out->len - 1] == ' ')
    g_string_truncate (out, out->len - 1);
  return g_string_free (out, FALSE);
}

/* ---------------------------------------------------------------------- */
/* MathML to text                                                          */
/* ---------------------------------------------------------------------- */

static void to_text (GString *out, const W42MathNode *n);

/* An argument, in brackets unless it is one thing. */
static void
text_arg (GString *out, const W42MathNode *n)
{
  GString *inner = g_string_new (NULL);

  to_text (inner, n);
  if (g_utf8_strlen (inner->str, -1) <= 1 ||
      (g_str_equal (n->name, "mn") || g_str_equal (n->name, "mi") || g_str_equal (n->name, "mo")))
    g_string_append (out, inner->str);
  else
    g_string_append_printf (out, "(%s)", inner->str);
  g_string_free (inner, TRUE);
}

static void
to_text (GString *out, const W42MathNode *n)
{
  const char *name = n->name;

  if (g_str_equal (name, "mo"))
    {
      const char *t = n->text != NULL ? n->text : "";
      gunichar c = g_utf8_get_char (t);
      char last = out->len > 0 ? out->str[out->len - 1] : '(';
      /* Space round a relation or an operator between two things, and
       * none after a sign in front of one. */
      gboolean spaced = *t != '\0' && *g_utf8_next_char (t) == '\0' &&
                        strstr ("=<>≤≥≠≈≡→←⇒⇔∈+−±×÷", t) != NULL && c != 0 &&
                        strchr ("([{ ,;", last) == NULL;

      if (c == 0x2061 || c == 0x2062 || c == 0x2063)
        {
          if (c == 0x2061) g_string_append_c (out, ' ');
          return;
        }
      if (spaced && out->len > 0) g_string_append_c (out, ' ');
      g_string_append (out, g_str_equal (t, "-") ? "−" : t);
      if (spaced) g_string_append_c (out, ' ');
      return;
    }
  if (n->text != NULL && (g_str_equal (name, "mi") || g_str_equal (name, "mn") ||
                          g_str_equal (name, "mtext") || g_str_equal (name, "ms")))
    {
      g_string_append (out, n->text);
      return;
    }
  if (g_str_equal (name, "mfrac") && n->children->len == 2)
    {
      text_arg (out, kid (n, 0));
      g_string_append_c (out, '/');
      text_arg (out, kid (n, 1));
      return;
    }
  if (g_str_equal (name, "msup") && n->children->len == 2 &&
      g_str_equal (kid (n, 1)->name, "mo") && kid (n, 1)->text != NULL &&
      (g_str_equal (kid (n, 1)->text, "′") || g_str_equal (kid (n, 1)->text, "″")))
    {
      text_arg (out, kid (n, 0));
      g_string_append (out, kid (n, 1)->text);
      return;
    }
  if ((g_str_equal (name, "mover") || g_str_equal (name, "munder")) && n->children->len == 2 &&
      g_str_equal (kid (n, 1)->name, "mo") && kid (n, 1)->text != NULL &&
      *kid (n, 1)->text != '\0' && *g_utf8_next_char (kid (n, 1)->text) == '\0' &&
      strstr ("^ˆ~˜¯‾→˙¨´`˘ˇ_⏞⏟", kid (n, 1)->text) != NULL)
    {
      /* An accent: over a letter, the combining one; over more, dropped. */
      static const struct { const char *mark, *combining; } COMBINING[] = {
        { "^", "\314\202" }, { "ˆ", "\314\202" }, { "~", "\314\203" }, { "˜", "\314\203" },
        { "¯", "\314\204" }, { "‾", "\314\205" }, { "→", "\342\203\227" }, { "˙", "\314\207" },
        { "¨", "\314\210" }, { "´", "\314\201" }, { "`", "\314\200" }, { "˘", "\314\206" },
        { "ˇ", "\314\214" }, { "_", "\314\262" },
      };
      GString *inner = g_string_new (NULL);

      to_text (inner, kid (n, 0));
      g_string_append (out, inner->str);
      if (g_utf8_strlen (inner->str, -1) == 1)
        for (guint k = 0; k < G_N_ELEMENTS (COMBINING); k++)
          if (g_str_equal (COMBINING[k].mark, kid (n, 1)->text))
            g_string_append (out, COMBINING[k].combining);
      g_string_free (inner, TRUE);
      return;
    }
  if (g_str_equal (name, "msup") && n->children->len == 2)
    {
      text_arg (out, kid (n, 0));
      g_string_append_c (out, '^');
      text_arg (out, kid (n, 1));
      return;
    }
  if ((g_str_equal (name, "msub") || g_str_equal (name, "munder")) && n->children->len == 2)
    {
      text_arg (out, kid (n, 0));
      g_string_append_c (out, '_');
      text_arg (out, kid (n, 1));
      return;
    }
  if ((g_str_equal (name, "msubsup") || g_str_equal (name, "munderover")) && n->children->len == 3)
    {
      text_arg (out, kid (n, 0));
      g_string_append_c (out, '_');
      text_arg (out, kid (n, 1));
      g_string_append_c (out, '^');
      text_arg (out, kid (n, 2));
      /* A sum's or an integral's limits, and a space before what it is of. */
      if (g_str_equal (kid (n, 0)->name, "mo"))
        g_string_append_c (out, ' ');
      return;
    }
  if (g_str_equal (name, "msqrt"))
    {
      GString *inner = g_string_new (NULL);

      for (guint i = 0; i < n->children->len; i++)
        to_text (inner, kid (n, i));
      g_string_append_printf (out, g_utf8_strlen (inner->str, -1) == 1 ? "√%s" : "√(%s)", inner->str);
      g_string_free (inner, TRUE);
      return;
    }
  if (g_str_equal (name, "mroot") && n->children->len == 2)
    {
      text_arg (out, kid (n, 1));
      g_string_append (out, "√");
      text_arg (out, kid (n, 0));
      return;
    }
  if (g_str_equal (name, "mtable"))
    {
      for (guint r = 0; r < n->children->len; r++)
        {
          const W42MathNode *row = kid (n, r);

          if (r > 0) g_string_append (out, "; ");
          for (guint c = 0; c < row->children->len; c++)
            {
              if (c > 0) g_string_append (out, ", ");
              to_text (out, kid (row, c));
            }
        }
      return;
    }
  if (g_str_equal (name, "semantics"))
    {
      if (kid (n, 0)) to_text (out, kid (n, 0));
      return;
    }
  if (g_str_equal (name, "annotation") || g_str_equal (name, "annotation-xml") ||
      g_str_equal (name, "mphantom"))
    return;
  if (g_str_equal (name, "mspace"))
    {
      g_string_append_c (out, ' ');
      return;
    }
  for (guint i = 0; i < n->children->len; i++)
    to_text (out, kid (n, i));
}

char *
w42_mathml_to_text (const W42MathNode *root)
{
  GString *out = g_string_new (NULL);

  g_return_val_if_fail (root != NULL, NULL);
  to_text (out, root);
  g_strstrip (out->str);
  out->len = strlen (out->str);
  return g_string_free (out, FALSE);
}
