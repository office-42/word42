/* w42-vba.c - Word42 Basic: VBA's syntax, rewritten for MY-BASIC
 *
 * Copyright (C) 2026 Andreas Røsdal
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * This program is free software: you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by the Free
 * Software Foundation, either version 3 of the License, or (at your option)
 * any later version.  See the LICENSE file for the full text.
 *
 * The translation is a line at a time.  Each source line becomes one line
 * of MY-BASIC, its statements joined with colons where one line has to
 * become two, so that the engine's row numbers map back to the macro's
 * lines through one small table.  A first pass over the source collects
 * the names of the Subs and Functions, since a call to one has to be
 * spelt `call name(...)` for MY-BASIC to find a routine defined further
 * down.
 *
 * What the dialect does not do -- For Each, GoTo, user classes, objects
 * held in variables -- stops the translation with a message naming the
 * line, rather than letting the engine report something stranger.
 */

#include "w42-vba.h"

#include <glib/gi18n.h>
#include <string.h>
#include <ctype.h>
#include <stdlib.h>

/* ---------------------------------------------------------------------- */
/* Tokens                                                                  */
/* ---------------------------------------------------------------------- */

typedef enum {
  TOK_ID,      /* an identifier or keyword */
  TOK_NUM,
  TOK_STR,     /* text is the MY-BASIC spelling, quotes included */
  TOK_OP       /* an operator or punctuation: text is the spelling */
} TokKind;

typedef struct {
  TokKind kind;
  char   *text;
} Tok;

static void
tok_clear (gpointer p)
{
  g_free (((Tok *) p)->text);
}

static gboolean
is_op (const Tok *t, const char *op)
{
  return t->kind == TOK_OP && g_str_equal (t->text, op);
}

static gboolean
is_kw (const Tok *t, const char *kw)
{
  return t->kind == TOK_ID && g_ascii_strcasecmp (t->text, kw) == 0;
}

/* A VBA string literal into MY-BASIC's, which has no escapes: a doubled
 * quote inside becomes a chr(34) joined in. */
static char *
string_literal (const char *body, gsize len)
{
  GString *out = g_string_new ("\"");
  gboolean open = TRUE;

  for (gsize i = 0; i < len; i++)
    {
      if (body[i] == '"')
        {
          /* Always a doubled quote here: the tokenizer stops at a lone one. */
          if (open)
            g_string_append (out, "\" + chr(34) + \"");
          i++;
        }
      else
        {
          g_string_append_c (out, body[i]);
        }
    }
  g_string_append_c (out, '"');

  /* "" + chr(34) + "..." reads oddly but is correct; an empty run at the
   * ends is trimmed for the engine's sake all the same. */
  if (g_str_has_prefix (out->str, "\"\" + "))
    g_string_erase (out, 0, 5);
  if (g_str_has_suffix (out->str, " + \"\""))
    g_string_truncate (out, out->len - 5);
  return g_string_free (out, FALSE);
}

/* One logical line into tokens.  The comment, if any, is dropped. */
static GArray *
tokenize (const char *line)
{
  GArray *toks = g_array_new (FALSE, FALSE, sizeof (Tok));
  const char *p = line;

  g_array_set_clear_func (toks, tok_clear);

  while (*p != '\0')
    {
      Tok t;

      if (g_ascii_isspace (*p))
        {
          p++;
          continue;
        }
      if (*p == '\'')
        break;                          /* a comment runs to the end */

      if (*p == '"')
        {
          const char *q = p + 1;

          for (;;)
            {
              if (*q == '\0')
                break;
              if (*q == '"')
                {
                  if (q[1] == '"')
                    {
                      q += 2;
                      continue;
                    }
                  break;
                }
              q++;
            }
          t.kind = TOK_STR;
          t.text = string_literal (p + 1, (gsize) (q - (p + 1)));
          g_array_append_val (toks, t);
          p = (*q == '"') ? q + 1 : q;
          continue;
        }

      if (g_ascii_isdigit (*p) || (*p == '.' && g_ascii_isdigit (p[1])))
        {
          const char *q = p;

          while (g_ascii_isalnum (*q) || *q == '.')
            {
              /* 1E-3: the sign belongs to the exponent. */
              if ((*q == 'e' || *q == 'E') && (q[1] == '-' || q[1] == '+') &&
                  g_ascii_isdigit (q[2]))
                q += 2;
              else
                q++;
            }
          t.kind = TOK_NUM;
          t.text = g_strndup (p, (gsize) (q - p));
          /* A type suffix (1%, 2&, 3#, 4!) says nothing to MY-BASIC. */
          while (*q == '%' || *q == '&' || *q == '#' || *q == '!' || *q == '@')
            q++;
          g_array_append_val (toks, t);
          p = q;
          continue;
        }

      if (*p == '&' && (p[1] == 'H' || p[1] == 'h'))
        {
          /* A hexadecimal literal, &H1F. */
          char *end = NULL;
          long v = strtol (p + 2, &end, 16);

          t.kind = TOK_NUM;
          t.text = g_strdup_printf ("%ld", v);
          g_array_append_val (toks, t);
          p = end;
          while (*p == '&')
            p++;
          continue;
        }

      if (g_ascii_isalpha (*p) || *p == '_')
        {
          const char *q = p;

          while (g_ascii_isalnum (*q) || *q == '_')
            q++;
          t.kind = TOK_ID;
          t.text = g_strndup (p, (gsize) (q - p));
          /* Left$, Mid$, Str$: the suffix is the type, not the name. */
          if (*q == '$' || *q == '%' || *q == '!' || *q == '#')
            q++;
          if (g_ascii_strcasecmp (t.text, "Rem") == 0)
            {
              g_free (t.text);
              break;                    /* a comment too */
            }
          g_array_append_val (toks, t);
          p = q;
          continue;
        }

      /* Operators: the two-character ones first. */
      {
        static const char *const two[] = { "<>", "<=", ">=", ":=", "=<", "=>", "><", NULL };
        const char *found = NULL;

        for (int i = 0; two[i] != NULL; i++)
          if (p[0] == two[i][0] && p[1] == two[i][1])
            {
              found = two[i];
              break;
            }
        t.kind = TOK_OP;
        if (found != NULL)
          {
            if (g_str_equal (found, "=<")) found = "<=";
            if (g_str_equal (found, "=>")) found = ">=";
            if (g_str_equal (found, "><")) found = "<>";
            t.text = g_strdup (found);
            p += 2;
          }
        else
          {
            t.text = g_strndup (p, 1);
            p++;
          }
        g_array_append_val (toks, t);
      }
    }

  return toks;
}

#define TOK(a, i) (&g_array_index ((a), Tok, (i)))

/* ---------------------------------------------------------------------- */
/* The translator's state                                                  */
/* ---------------------------------------------------------------------- */

typedef enum {
  BLK_SUB,
  BLK_FUNCTION,
  BLK_IF,
  BLK_FOR,
  BLK_WHILE,     /* Do While / Do Until / While: closes with wend */
  BLK_DO,        /* Do ... Loop [While|Until]: closes with until */
  BLK_SELECT,
  BLK_WITH
} BlockKind;

typedef struct {
  BlockKind kind;
  char     *name;      /* the function's name; a With's object chain */
  gboolean  first;     /* a Select's first Case is still to come */
  int       serial;    /* a Select's variable number */
} Block;

typedef struct {
  GString    *out;
  GArray     *line_map;      /* int per output line */
  GHashTable *subs;          /* lower-case name -> the name as declared */
  GPtrArray  *sub_names;
  GArray     *blocks;        /* Block */
  int         source_line;   /* the line being translated, 1-based */
  int         selects;       /* Select Case blocks seen, for their variables */
  char       *error;
} State;

static void
fail (State *st, const char *what)
{
  if (st->error == NULL)
    /* Translators: %d is a line of the macro, %s what is wrong with it. */
    st->error = g_strdup_printf (_("Line %d: %s"), st->source_line, what);
}

static Block *
top_block (State *st)
{
  return st->blocks->len > 0 ? &g_array_index (st->blocks, Block, st->blocks->len - 1) : NULL;
}

static void
push_block (State *st, BlockKind kind, const char *name)
{
  Block b = { kind, g_strdup (name), TRUE, 0 };

  g_array_append_val (st->blocks, b);
}

static void
pop_block (State *st)
{
  if (st->blocks->len > 0)
    {
      g_free (top_block (st)->name);
      g_array_set_size (st->blocks, st->blocks->len - 1);
    }
}

/* The innermost function being translated, or NULL. */
static const Block *
enclosing_function (State *st)
{
  for (guint i = st->blocks->len; i > 0; i--)
    {
      const Block *b = &g_array_index (st->blocks, Block, i - 1);

      if (b->kind == BLK_FUNCTION)
        return b;
      if (b->kind == BLK_SUB)
        return NULL;
    }
  return NULL;
}

/* The innermost With, or NULL. */
static const char *
with_prefix (State *st)
{
  for (guint i = st->blocks->len; i > 0; i--)
    {
      const Block *b = &g_array_index (st->blocks, Block, i - 1);

      if (b->kind == BLK_WITH)
        return b->name;
      if (b->kind == BLK_SUB || b->kind == BLK_FUNCTION)
        break;
    }
  return NULL;
}

/* VBA's reserved words, none of which can be a label: "Else:" at the
 * start of a line is Else, and the colon ends it. */
static gboolean
is_reserved (const char *id)
{
  static const char *const words[] = {
    "And", "As", "ByRef", "ByVal", "Call", "Case", "Const", "Declare", "Dim",
    "Do", "Each", "Else", "ElseIf", "End", "Enum", "Erase", "Exit", "False",
    "For", "Function", "Get", "Global", "GoSub", "GoTo", "If", "Is", "Let",
    "Like", "Loop", "Me", "Mod", "New", "Next", "Not", "Nothing", "On",
    "Option", "Optional", "Or", "Private", "Property", "Public", "ReDim",
    "Rem", "Resume", "Return", "Select", "Set", "Static", "Stop", "Sub",
    "Then", "To", "True", "Type", "Until", "Wend", "While", "With", "Xor",
    NULL
  };

  for (int i = 0; words[i] != NULL; i++)
    if (g_ascii_strcasecmp (id, words[i]) == 0)
      return TRUE;
  return FALSE;
}

static gboolean
is_user_sub (State *st, const char *name)
{
  char *key = g_ascii_strdown (name, -1);
  gboolean yes = g_hash_table_contains (st->subs, key);

  g_free (key);
  return yes;
}

/* ---------------------------------------------------------------------- */
/* Expressions                                                             */
/* ---------------------------------------------------------------------- */

static void translate_expr (State *st, GArray *toks, guint from, guint to, GPtrArray *atoms);

/* Named arguments are reordered by what the method takes: Word's
 * signatures, for the members Word42 has. */
typedef struct {
  const char *member;
  const char *params[12];
} Signature;

static const Signature SIGNATURES[] = {
  { "msgbox",     { "prompt", "buttons", "title", NULL } },
  { "inputbox",   { "prompt", "title", "default", NULL } },
  { "typetext",   { "text", NULL } },
  { "insertafter", { "text", NULL } },
  { "insertbefore", { "text", NULL } },
  { "delete",     { "unit", "count", NULL } },
  { "moveleft",   { "unit", "count", "extend", NULL } },
  { "moveright",  { "unit", "count", "extend", NULL } },
  { "moveup",     { "unit", "count", "extend", NULL } },
  { "movedown",   { "unit", "count", "extend", NULL } },
  { "movestart",  { "unit", "count", NULL } },
  { "moveend",    { "unit", "count", NULL } },
  { "homekey",    { "unit", "extend", NULL } },
  { "endkey",     { "unit", "extend", NULL } },
  { "collapse",   { "direction", NULL } },
  { "execute",    { "findtext", "matchcase", "matchwholeword", "matchwildcards",
                    "matchsoundslike", "matchallwordforms", "forward", "wrap",
                    "format", "replacewith", "replace", NULL } },
  { "saveas",     { "filename", "fileformat", NULL } },
  { "saveas2",    { "filename", "fileformat", NULL } },
  { "open",       { "filename", NULL } },
  { "add",        { "template", NULL } },
  { "close",      { "savechanges", NULL } },
  { "printout",   { "background", "append", "range", "outputfilename", "from", "to", NULL } },
  { "computestatistics", { "statistic", NULL } },
  { "insertbreak", { "type", NULL } },
  { "goto",       { "what", "which", "count", "name", NULL } },
  { "insertparagraph", { NULL } },
  { "replace",    { "expression", "find", "replace", NULL } },
  { "instr",      { "start", "string1", "string2", NULL } },
  { "mid",        { "string", "start", "length", NULL } },
  { NULL,         { NULL } }
};

static const Signature *
find_signature (const char *member)
{
  for (int i = 0; SIGNATURES[i].member != NULL; i++)
    if (g_ascii_strcasecmp (SIGNATURES[i].member, member) == 0)
      return &SIGNATURES[i];
  return NULL;
}

/* The index of the token closing the bracket opened at `open`, or `to`
 * when it never closes. */
static guint
matching_close (GArray *toks, guint open, guint to)
{
  int depth = 0;

  for (guint i = open; i < to; i++)
    {
      if (is_op (TOK (toks, i), "("))
        depth++;
      else if (is_op (TOK (toks, i), ")"))
        {
          depth--;
          if (depth == 0)
            return i;
        }
    }
  return to;
}

static char *
atoms_join (GPtrArray *atoms, guint from, guint to)
{
  GString *s = g_string_new (NULL);

  for (guint i = from; i < to && i < atoms->len; i++)
    {
      const char *a = g_ptr_array_index (atoms, i);

      if (s->len > 0 && !g_str_equal (a, ")") && !g_str_equal (a, ",") &&
          !g_str_has_suffix (s->str, "("))
        g_string_append_c (s, ' ');
      g_string_append (s, a);
    }
  return g_string_free (s, FALSE);
}

/* The arguments of a call, [from, to) being the tokens between its
 * brackets, translated and put in the order `member` takes them. */
static char *
translate_args (State *st, GArray *toks, guint from, guint to, const char *member)
{
  GPtrArray *positional = g_ptr_array_new_with_free_func (g_free);
  GPtrArray *named_keys = g_ptr_array_new_with_free_func (g_free);
  GPtrArray *named_vals = g_ptr_array_new_with_free_func (g_free);
  guint start = from;
  int depth = 0;
  char *result;

  for (guint i = from; i <= to; i++)
    {
      gboolean end = (i == to);

      if (!end)
        {
          const Tok *t = TOK (toks, i);

          if (is_op (t, "("))
            depth++;
          else if (is_op (t, ")"))
            depth--;
          else if (depth == 0 && (is_op (t, ",") || is_op (t, ";")))
            end = TRUE;
        }
      if (!end)
        continue;

      if (i > start)
        {
          GPtrArray *atoms = g_ptr_array_new_with_free_func (g_free);
          guint expr_from = start;
          char *name = NULL;

          if (i - start >= 3 && TOK (toks, start)->kind == TOK_ID &&
              is_op (TOK (toks, start + 1), ":="))
            {
              name = g_ascii_strdown (TOK (toks, start)->text, -1);
              expr_from = start + 2;
            }
          translate_expr (st, toks, expr_from, i, atoms);
          if (name != NULL)
            {
              g_ptr_array_add (named_keys, name);
              g_ptr_array_add (named_vals, atoms_join (atoms, 0, atoms->len));
            }
          else
            {
              g_ptr_array_add (positional, atoms_join (atoms, 0, atoms->len));
            }
          g_ptr_array_unref (atoms);
        }
      else if (!end || i < to)
        {
          /* An argument left out: Foo a, , c. */
          g_ptr_array_add (positional, g_strdup ("nil"));
        }
      start = i + 1;
    }

  if (named_keys->len > 0)
    {
      const Signature *sig = member != NULL ? find_signature (member) : NULL;
      GPtrArray *slots = g_ptr_array_new_with_free_func (g_free);
      guint n_params = 0;

      if (sig != NULL)
        while (sig->params[n_params] != NULL)
          n_params++;
      for (guint i = 0; i < MAX (n_params, positional->len); i++)
        g_ptr_array_add (slots, g_strdup (i < positional->len
                                          ? g_ptr_array_index (positional, i) : "nil"));
      for (guint k = 0; k < named_keys->len; k++)
        {
          const char *key = g_ptr_array_index (named_keys, k);
          guint slot = G_MAXUINT;

          for (guint p = 0; p < n_params; p++)
            if (g_str_equal (sig->params[p], key))
              slot = p;
          if (slot == G_MAXUINT)
            {
              /* Not a name the method knows: taken in the order written. */
              g_ptr_array_add (slots, g_strdup (g_ptr_array_index (named_vals, k)));
              continue;
            }
          g_free (g_ptr_array_index (slots, slot));
          g_ptr_array_index (slots, slot) = g_strdup (g_ptr_array_index (named_vals, k));
        }
      /* The nils at the end say nothing. */
      while (slots->len > 0 && g_str_equal (g_ptr_array_index (slots, slots->len - 1), "nil"))
        g_ptr_array_set_size (slots, slots->len - 1);
      g_ptr_array_unref (positional);
      positional = slots;
    }

  {
    GString *s = g_string_new (NULL);

    for (guint i = 0; i < positional->len; i++)
      {
        if (i > 0)
          g_string_append (s, ", ");
        g_string_append (s, g_ptr_array_index (positional, i));
      }
    result = g_string_free (s, FALSE);
  }
  g_ptr_array_unref (positional);
  g_ptr_array_unref (named_keys);
  g_ptr_array_unref (named_vals);
  return result;
}

/* A dotted chain starting at `i`: Selection.Font.Bold.  Returns the chain
 * joined with underscores and sets `*next` to the token after it.  A
 * chain starting with a dot takes the With's object first. */
static char *
read_chain (State *st, GArray *toks, guint i, guint to, guint *next)
{
  GString *s = g_string_new (NULL);
  guint j = i;

  if (is_op (TOK (toks, i), "."))
    {
      const char *with = with_prefix (st);

      if (with == NULL)
        {
          /* Translators: "With" is a macro keyword: keep it in English. */
          fail (st, _("a name starting with a dot needs a With block"));
          g_string_free (s, TRUE);
          *next = i + 1;
          return g_strdup ("nil");
        }
      g_string_append (s, with);
      /* j stays: the loop below takes the dot and the member. */
    }
  else
    {
      g_string_append (s, TOK (toks, j)->text);
      j++;
    }

  while (j + 1 < to && is_op (TOK (toks, j), ".") && TOK (toks, j + 1)->kind == TOK_ID)
    {
      g_string_append_c (s, '_');
      g_string_append (s, TOK (toks, j + 1)->text);
      j += 2;
    }
  *next = j;

  /* Application.Selection is Selection; Application.ActiveDocument too. */
  if (g_ascii_strncasecmp (s->str, "Application_Selection", 21) == 0 ||
      g_ascii_strncasecmp (s->str, "Application_ActiveDocument", 26) == 0 ||
      g_ascii_strncasecmp (s->str, "Application_Documents", 21) == 0)
    g_string_erase (s, 0, 12);
  /* ActiveDocument.Content and .Range are the whole document. */
  if (g_ascii_strncasecmp (s->str, "ActiveDocument_Content_", 23) == 0)
    g_string_erase (s, 15, 8);
  else if (g_ascii_strncasecmp (s->str, "ActiveDocument_Range_", 21) == 0)
    g_string_erase (s, 15, 6);
  else if (g_ascii_strcasecmp (s->str, "ActiveDocument_Content") == 0 ||
           g_ascii_strcasecmp (s->str, "ActiveDocument_Range") == 0)
    g_string_assign (s, "ActiveDocument_Text");
  return g_string_free (s, FALSE);
}

static const char *
chain_member (const char *chain)
{
  const char *u = strrchr (chain, '_');

  return u != NULL ? u + 1 : chain;
}

/* VBA's functions of no arguments, written without brackets --
 * Selection.TypeText Date -- which MY-BASIC will not call without. */
static gboolean
is_bare_function (const char *id)
{
  return g_ascii_strcasecmp (id, "Date") == 0 || g_ascii_strcasecmp (id, "Now") == 0 ||
         g_ascii_strcasecmp (id, "Time") == 0 || g_ascii_strcasecmp (id, "Timer") == 0;
}

/* Words MY-BASIC spells differently, or has as keywords of its own. */
static const char *
keyword_spelling (const char *id)
{
  static const struct { const char *vba, *mb; } map[] = {
    { "True", "true" }, { "False", "false" }, { "Nothing", "nil" },
    { "Null", "nil" }, { "Empty", "nil" }, { "Mod", "mod" },
    { "And", "and" }, { "Or", "or" }, { "Not", "not" },
    { "Then", "then" }, { "Else", "else" }, { "To", "to" }, { "Step", "step" },
    { "Xor", "XOR_" }, { "Eqv", "EQV_" }, { "Like", "LIKE_" },
    { NULL, NULL }
  };

  for (int i = 0; map[i].vba != NULL; i++)
    if (g_ascii_strcasecmp (id, map[i].vba) == 0)
      return map[i].mb;
  return NULL;
}

/* Is this atom a boundary for a binary rewrite: something an operand
 * of & cannot run across? */
static gboolean
is_boundary (const char *a, gboolean arithmetic_too)
{
  static const char *const words[] = { "=", "<>", "<", ">", "<=", ">=", ",",
                                       "and", "or", "not", "then", "else", "to",
                                       "step", "is", NULL };

  for (int i = 0; words[i] != NULL; i++)
    if (g_ascii_strcasecmp (a, words[i]) == 0)
      return TRUE;
  /* Only what binds more loosely than the operator ends an operand: ^
   * binds more tightly than \, so 4 ^ 2 \ 3 is (4 ^ 2) \ 3, as it is
   * with * and /. */
  if (arithmetic_too)
    return g_str_equal (a, "+") || g_str_equal (a, "-") || g_ascii_strcasecmp (a, "mod") == 0
        || g_str_equal (a, "&") || g_str_equal (a, "\\");
  return g_str_equal (a, "&");
}

/* Rewrites every `left OP right` at the atoms' top level into
 * `fn(left, right)`, walking outwards from the operator to the nearest
 * boundary or unmatched bracket on either side. */
static void
rewrite_binary (GPtrArray *atoms, const char *op, const char *fn, gboolean arithmetic_bounds)
{
  for (guint i = 0; i < atoms->len; i++)
    {
      guint l, r;
      int depth;
      char *left, *right, *joined;

      if (!g_str_equal (g_ptr_array_index (atoms, i), op))
        continue;

      /* Back to the operand's start. */
      depth = 0;
      l = i;
      while (l > 0)
        {
          const char *a = g_ptr_array_index (atoms, l - 1);

          if (g_str_equal (a, ")"))
            depth++;
          else if (g_str_equal (a, "("))
            {
              if (depth == 0)
                break;
              depth--;
            }
          else if (depth == 0 && is_boundary (a, arithmetic_bounds))
            break;
          l--;
        }
      /* Forward to the operand's end.  A sign right after the operator
       * belongs to the operand: 10 \ -3 is 10 \ (-3), not (10 \ ) - 3. */
      depth = 0;
      r = i + 1;
      if (r < atoms->len &&
          (g_str_equal (g_ptr_array_index (atoms, r), "-") ||
           g_str_equal (g_ptr_array_index (atoms, r), "+")))
        r++;
      while (r < atoms->len)
        {
          const char *a = g_ptr_array_index (atoms, r);

          if (g_str_equal (a, "("))
            depth++;
          else if (g_str_equal (a, ")"))
            {
              if (depth == 0)
                break;
              depth--;
            }
          else if (depth == 0 && is_boundary (a, arithmetic_bounds))
            break;
          r++;
        }

      left = atoms_join (atoms, l, i);
      right = atoms_join (atoms, i + 1, r);
      joined = g_strdup_printf ("%s(%s, %s)", fn, left, right);
      g_free (left);
      g_free (right);
      g_ptr_array_remove_range (atoms, l, r - l);
      g_ptr_array_insert (atoms, (int) l, joined);
      i = l;
    }
}

/* Translates the tokens [from, to) as an expression, appending atoms:
 * each a translated call, name, literal, bracket or operator. */
static void
translate_expr (State *st, GArray *toks, guint from, guint to, GPtrArray *atoms)
{
  guint i = from;

  while (i < to)
    {
      const Tok *t = TOK (toks, i);

      if (t->kind == TOK_ID || (is_op (t, ".") && i + 1 < to && TOK (toks, i + 1)->kind == TOK_ID))
        {
          const char *kw = t->kind == TOK_ID ? keyword_spelling (t->text) : NULL;
          guint next;
          char *chain;
          gboolean dotted;

          if (kw != NULL && !(i + 1 < to && is_op (TOK (toks, i + 1), ".")))
            {
              g_ptr_array_add (atoms, g_strdup (kw));
              i++;
              continue;
            }

          chain = read_chain (st, toks, i, to, &next);
          dotted = strchr (chain, '_') != NULL && !g_str_has_prefix (chain, "_");
          i = next;

          if (i < to && is_op (TOK (toks, i), "("))
            {
              guint close = matching_close (toks, i, to);
              char *args = translate_args (st, toks, i + 1, close, chain_member (chain));
              const char *prefix = is_user_sub (st, chain) ? "call " : "";

              g_ptr_array_add (atoms, g_strdup_printf ("%s%s(%s)", prefix, chain, args));
              g_free (args);
              i = close + 1;
            }
          else if (dotted || is_user_sub (st, chain) || is_bare_function (chain))
            {
              /* A property read, or a function of no arguments. */
              g_ptr_array_add (atoms, g_strdup_printf ("%s%s()", is_user_sub (st, chain) ? "call " : "", chain));
            }
          else
            {
              g_ptr_array_add (atoms, g_strdup (chain));
            }
          g_free (chain);
          continue;
        }

      if (t->kind == TOK_OP && is_op (t, ":="))
        {
          fail (st, _("a named argument outside a call"));
          i++;
          continue;
        }

      g_ptr_array_add (atoms, g_strdup (t->text));
      i++;
    }

  rewrite_binary (atoms, "\\", "IDIV", TRUE);
  rewrite_binary (atoms, "&", "CAT", FALSE);
}

static char *
expr_string (State *st, GArray *toks, guint from, guint to)
{
  GPtrArray *atoms = g_ptr_array_new_with_free_func (g_free);
  char *s;

  translate_expr (st, toks, from, to, atoms);
  s = atoms_join (atoms, 0, atoms->len);
  g_ptr_array_unref (atoms);
  return s;
}

/* ---------------------------------------------------------------------- */
/* Statements                                                              */
/* ---------------------------------------------------------------------- */

static void translate_statement (State *st, GArray *toks, guint from, guint to, GString *out);

/* The index of the first top-level `Then` in [from, to), or `to`. */
static guint
find_kw (GArray *toks, guint from, guint to, const char *kw)
{
  int depth = 0;

  for (guint i = from; i < to; i++)
    {
      const Tok *t = TOK (toks, i);

      if (is_op (t, "("))
        depth++;
      else if (is_op (t, ")"))
        depth--;
      else if (depth == 0 && is_kw (t, kw))
        return i;
    }
  return to;
}

/* Sub name(params) / Function name(params) As Type: the def line. */
static void
translate_procedure (State *st, GArray *toks, guint from, guint to, gboolean function, GString *out)
{
  const char *name;
  GString *params = g_string_new (NULL);
  guint i;

  if (from >= to || TOK (toks, from)->kind != TOK_ID)
    {
      /* Translators: Sub and Function are macro keywords: keep them in
       * English. */
      fail (st, _("Sub or Function without a name"));
      return;
    }
  name = TOK (toks, from)->text;
  i = from + 1;
  if (i < to && is_op (TOK (toks, i), "("))
    {
      guint close = matching_close (toks, i, to);
      gboolean expect_name = TRUE;

      for (guint k = i + 1; k < close; k++)
        {
          const Tok *t = TOK (toks, k);

          if (is_op (t, ","))
            {
              expect_name = TRUE;
              continue;
            }
          if (t->kind != TOK_ID)
            continue;
          if (is_kw (t, "ByVal") || is_kw (t, "ByRef") || is_kw (t, "Optional") || is_kw (t, "ParamArray"))
            continue;
          if (is_kw (t, "As"))
            {
              k++;                      /* the type */
              continue;
            }
          if (expect_name)
            {
              if (params->len > 0)
                g_string_append (params, ", ");
              g_string_append (params, t->text);
              expect_name = FALSE;
              /* An Optional's default is not something MY-BASIC does. */
              if (k + 1 < close && is_op (TOK (toks, k + 1), "="))
                /* Translators: "Optional" is a macro keyword: keep it in
                 * English. */
                fail (st, _("an Optional parameter with a default value is not supported"));
            }
        }
    }
  g_string_append_printf (out, "def %s(%s)", name, params->str);
  if (function)
    g_string_append_printf (out, " : __ret_%s = 0", name);
  push_block (st, function ? BLK_FUNCTION : BLK_SUB, name);
  g_string_free (params, TRUE);
}

/* Dim x As T, y(3) As T: each variable gets its type's nothing. */
static void
translate_dim (State *st, GArray *toks, guint from, guint to, GString *out)
{
  guint i = from;
  gboolean first = TRUE;

  while (i < to)
    {
      const Tok *t = TOK (toks, i);
      const char *type = NULL;
      char *name;
      guint dims_from = 0, dims_to = 0;

      if (t->kind != TOK_ID)
        {
          /* Translators: "Dim" is a macro keyword: keep it in English. */
          fail (st, _("Dim expects a name"));
          return;
        }
      name = t->text;
      i++;
      if (i < to && is_op (TOK (toks, i), "("))
        {
          guint close = matching_close (toks, i, to);

          dims_from = i + 1;
          dims_to = close;
          i = close + 1;
        }
      if (i + 1 < to && is_kw (TOK (toks, i), "As"))
        {
          i++;
          if (is_kw (TOK (toks, i), "New"))
            {
              /* Translators: "New" is a macro keyword: keep it in English. */
              fail (st, _("objects made with New are not supported"));
              return;
            }
          type = TOK (toks, i)->text;
          i++;
        }
      if (!first)
        g_string_append (out, " : ");
      first = FALSE;
      if (dims_to > dims_from)
        {
          /* Dim a(10) is eleven elements, 0 to 10; dim a(11) is the same
           * in MY-BASIC.  Each dimension gets one more. */
          GString *dims = g_string_new (NULL);
          guint start = dims_from;
          int depth = 0;

          for (guint k = dims_from; k <= dims_to; k++)
            {
              gboolean end = (k == dims_to);

              if (!end)
                {
                  if (is_op (TOK (toks, k), "(")) depth++;
                  else if (is_op (TOK (toks, k), ")")) depth--;
                  else if (depth == 0 && is_op (TOK (toks, k), ",")) end = TRUE;
                }
              if (!end)
                continue;
              {
                guint lo_to = find_kw (toks, start, k, "To");
                guint hi = lo_to < k ? lo_to + 1 : start;

                /* MY-BASIC sizes an array by a literal alone. */
                if (hi + 1 != k || TOK (toks, hi)->kind != TOK_NUM)
                  {
                    /* Translators: "Dim a(10)" is macro code: keep it as it
                     * is. */
                    fail (st, _("an array's size must be a number: Dim a(10)"));
                    g_string_free (dims, TRUE);
                    return;
                  }
                if (dims->len > 0)
                  g_string_append (dims, ", ");
                g_string_append_printf (dims, "%d", atoi (TOK (toks, hi)->text) + 1);
              }
              start = k + 1;
            }
          g_string_append_printf (out, "dim %s(%s)", name, dims->str);
          g_string_free (dims, TRUE);
        }
      else if (dims_from > 0)
        {
          /* Dim a() As String: sized later by ReDim. */
          g_string_append_printf (out, "%s = nil", name);
        }
      else
        {
          const char *zero = "0";

          if (type != NULL && g_ascii_strcasecmp (type, "String") == 0)
            zero = "\"\"";
          else if (type != NULL && g_ascii_strcasecmp (type, "Boolean") == 0)
            zero = "false";
          else if (type != NULL && (g_ascii_strcasecmp (type, "Object") == 0 ||
                                    g_ascii_strcasecmp (type, "Variant") == 0 ||
                                    g_ascii_strcasecmp (type, "Range") == 0 ||
                                    g_ascii_strcasecmp (type, "Document") == 0 ||
                                    g_ascii_strcasecmp (type, "Selection") == 0))
            zero = "nil";
          g_string_append_printf (out, "%s = %s", name, zero);
        }
      if (i < to && is_op (TOK (toks, i), ","))
        i++;
    }
}

/* Case 1, 3 To 5, Is > 9: one condition on the Select's variable. */
static char *
translate_case (State *st, GArray *toks, guint from, guint to, int serial)
{
  GString *cond = g_string_new (NULL);
  guint start = from;
  int depth = 0;

  for (guint k = from; k <= to; k++)
    {
      gboolean end = (k == to);

      if (!end)
        {
          if (is_op (TOK (toks, k), "(")) depth++;
          else if (is_op (TOK (toks, k), ")")) depth--;
          else if (depth == 0 && is_op (TOK (toks, k), ",")) end = TRUE;
        }
      if (!end)
        continue;
      if (cond->len > 0)
        g_string_append (cond, " or ");
      if (start < k && is_kw (TOK (toks, start), "Is") && start + 1 < k)
        {
          char *e = expr_string (st, toks, start + 2, k);

          g_string_append_printf (cond, "(__sel%d %s %s)", serial, TOK (toks, start + 1)->text, e);
          g_free (e);
        }
      else
        {
          guint t = find_kw (toks, start, k, "To");

          if (t < k)
            {
              char *lo = expr_string (st, toks, start, t);
              char *hi = expr_string (st, toks, t + 1, k);

              g_string_append_printf (cond, "(__sel%d >= %s and __sel%d <= %s)", serial, lo, serial, hi);
              g_free (lo);
              g_free (hi);
            }
          else
            {
              char *e = expr_string (st, toks, start, k);

              g_string_append_printf (cond, "(__sel%d = %s)", serial, e);
              g_free (e);
            }
        }
      start = k + 1;
    }
  return g_string_free (cond, FALSE);
}

static void
translate_statement (State *st, GArray *toks, guint from, guint to, GString *out)
{
  const Tok *t;

  if (from >= to)
    return;
  t = TOK (toks, from);

  /* Declarations' decorations. */
  while (from < to && (is_kw (t, "Private") || is_kw (t, "Public") || is_kw (t, "Friend") ||
                       is_kw (t, "Static") || is_kw (t, "Global")))
    {
      from++;
      if (from >= to)
        return;
      t = TOK (toks, from);
    }

  if (t->kind == TOK_ID)
    {
      const char *k = t->text;

      if (g_ascii_strcasecmp (k, "Option") == 0 || g_ascii_strcasecmp (k, "Attribute") == 0 ||
          g_ascii_strcasecmp (k, "DefInt") == 0 || g_ascii_strcasecmp (k, "DefStr") == 0 ||
          g_ascii_strcasecmp (k, "Beep") == 0 || g_ascii_strcasecmp (k, "DoEvents") == 0)
        return;
      if (g_ascii_strcasecmp (k, "On") == 0)
        return;                         /* On Error: there is no error trapping */
      if (g_ascii_strcasecmp (k, "Sub") == 0)
        {
          translate_procedure (st, toks, from + 1, to, FALSE, out);
          return;
        }
      if (g_ascii_strcasecmp (k, "Function") == 0)
        {
          translate_procedure (st, toks, from + 1, to, TRUE, out);
          return;
        }
      if (g_ascii_strcasecmp (k, "End") == 0)
        {
          const Block *b = top_block (st);

          if (from + 1 >= to)
            {
              g_string_append (out, "end");
              return;
            }
          t = TOK (toks, from + 1);
          if (is_kw (t, "Sub") || is_kw (t, "Function"))
            {
              static const char *const open[] = { "Sub", "Function", "If", "For", "Do", "Do", "Select Case", "With" };
              BlockKind want = is_kw (t, "Sub") ? BLK_SUB : BLK_FUNCTION;

              if (b == NULL)
                {
                  /* Translators: End Sub, Sub, End Function and Function
                   * are macro keywords: keep them in English. */
                  fail (st, is_kw (t, "Sub") ? _("End Sub without Sub")
                                             : _("End Function without Function"));
                  return;
                }
              if (b->kind != want)
                {
                  /* Translators: the first %s is a macro keyword (Sub,
                   * Function, If, For, Do, Select Case or With), the
                   * second Sub or Function; "End" is a keyword too.  Keep
                   * them all in English. */
                  char *msg = g_strdup_printf (_("%s is still open at End %s"),
                                               open[b->kind], t->text);

                  fail (st, msg);
                  g_free (msg);
                  return;
                }
              if (want == BLK_FUNCTION)
                g_string_append_printf (out, "return __ret_%s : enddef", b->name);
              else
                g_string_append (out, "enddef");
              pop_block (st);
            }
          else if (is_kw (t, "If"))
            {
              /* Translators: End If and If are macro keywords: keep them
               * in English. */
              if (b == NULL || b->kind != BLK_IF) { fail (st, _("End If without If")); return; }
              g_string_append (out, "endif");
              pop_block (st);
            }
          else if (is_kw (t, "Select"))
            {
              /* Translators: End Select and Select are macro keywords:
               * keep them in English. */
              if (b == NULL || b->kind != BLK_SELECT) { fail (st, _("End Select without Select")); return; }
              g_string_append (out, b->first ? "" : "endif");
              pop_block (st);
            }
          else if (is_kw (t, "With"))
            {
              /* Translators: End With and With are macro keywords: keep
               * them in English. */
              if (b == NULL || b->kind != BLK_WITH) { fail (st, _("End With without With")); return; }
              pop_block (st);
            }
          else
            /* Translators: "End" is a macro keyword: keep it in English. */
            fail (st, _("End of what?"));
          return;
        }
      if (g_ascii_strcasecmp (k, "Exit") == 0)
        {
          t = from + 1 < to ? TOK (toks, from + 1) : NULL;
          if (t != NULL && is_kw (t, "Sub"))
            g_string_append (out, "return");
          else if (t != NULL && is_kw (t, "Function"))
            {
              const Block *f = enclosing_function (st);

              g_string_append_printf (out, "return __ret_%s", f != NULL ? f->name : "");
            }
          else if (t != NULL && (is_kw (t, "Do") || is_kw (t, "For")))
            g_string_append (out, "exit");
          else
            /* Translators: "Exit" is a macro keyword: keep it in English. */
            fail (st, _("Exit of what?"));
          return;
        }
      if (g_ascii_strcasecmp (k, "If") == 0)
        {
          guint then = find_kw (toks, from + 1, to, "Then");
          char *cond;

          if (then >= to)
            {
              /* Translators: If and Then are macro keywords: keep them in
               * English. */
              fail (st, _("If without Then"));
              return;
            }
          cond = expr_string (st, toks, from + 1, then);
          if (then + 1 >= to)
            {
              g_string_append_printf (out, "if %s then", cond);
              push_block (st, BLK_IF, NULL);
            }
          else
            {
              /* One line: If c Then a [Else b]. */
              guint els = find_kw (toks, then + 1, to, "Else");
              GString *a = g_string_new (NULL);

              translate_statement (st, toks, then + 1, els, a);
              g_string_append_printf (out, "if %s then %s", cond, a->str);
              if (els < to)
                {
                  GString *b = g_string_new (NULL);

                  translate_statement (st, toks, els + 1, to, b);
                  g_string_append_printf (out, " else %s", b->str);
                  g_string_free (b, TRUE);
                }
              g_string_free (a, TRUE);
            }
          g_free (cond);
          return;
        }
      if (g_ascii_strcasecmp (k, "ElseIf") == 0)
        {
          guint then = find_kw (toks, from + 1, to, "Then");
          char *cond = expr_string (st, toks, from + 1, then);

          g_string_append_printf (out, "elseif %s then", cond);
          g_free (cond);
          return;
        }
      if (g_ascii_strcasecmp (k, "Else") == 0)
        {
          if (from + 1 < to && is_kw (TOK (toks, from + 1), "If"))
            {
              guint then = find_kw (toks, from + 2, to, "Then");
              char *cond = expr_string (st, toks, from + 2, then);

              g_string_append_printf (out, "elseif %s then", cond);
              g_free (cond);
            }
          else
            g_string_append (out, "else");
          return;
        }
      if (g_ascii_strcasecmp (k, "Select") == 0)
        {
          char *e;

          if (from + 1 >= to || !is_kw (TOK (toks, from + 1), "Case"))
            {
              /* Translators: Select and Case are macro keywords: keep them
               * in English. */
              fail (st, _("Select without Case"));
              return;
            }
          st->selects++;
          e = expr_string (st, toks, from + 2, to);
          g_string_append_printf (out, "__sel%d = %s", st->selects, e);
          g_free (e);
          push_block (st, BLK_SELECT, NULL);
          top_block (st)->serial = st->selects;
          return;
        }
      if (g_ascii_strcasecmp (k, "Case") == 0)
        {
          Block *b = top_block (st);

          if (b == NULL || b->kind != BLK_SELECT)
            {
              /* Translators: Case and Select Case are macro keywords: keep
               * them in English. */
              fail (st, _("Case outside Select Case"));
              return;
            }
          if (from + 1 < to && is_kw (TOK (toks, from + 1), "Else"))
            {
              g_string_append (out, b->first ? "if true then" : "else");
            }
          else
            {
              char *cond = translate_case (st, toks, from + 1, to, b->serial);

              g_string_append_printf (out, "%s %s then", b->first ? "if" : "elseif", cond);
              g_free (cond);
            }
          b->first = FALSE;
          return;
        }
      if (g_ascii_strcasecmp (k, "For") == 0)
        {
          if (from + 1 < to && is_kw (TOK (toks, from + 1), "Each"))
            {
              /* Translators: "For Each" is a macro keyword, and "For i = 1
               * To ...Count" macro code: keep them in English. */
              fail (st, _("For Each is not supported: count with For i = 1 To ...Count"));
              return;
            }
          {
            guint eq = from + 2, to_kw = find_kw (toks, from + 1, to, "To");
            guint step = find_kw (toks, from + 1, to, "Step");
            char *start, *end;

            if (from + 1 >= to || TOK (toks, from + 1)->kind != TOK_ID || to_kw >= to ||
                eq >= to || !is_op (TOK (toks, eq), "="))
              {
                /* Translators: "For" is a macro keyword, and "For i = a To
                 * b" macro code: keep them in English. */
                fail (st, _("For expects: For i = a To b"));
                return;
              }
            start = expr_string (st, toks, eq + 1, to_kw);
            end = expr_string (st, toks, to_kw + 1, step);
            g_string_append_printf (out, "for %s = %s to %s", TOK (toks, from + 1)->text, start, end);
            if (step < to)
              {
                char *s = expr_string (st, toks, step + 1, to);

                g_string_append_printf (out, " step %s", s);
                g_free (s);
              }
            g_free (start);
            g_free (end);
            push_block (st, BLK_FOR, NULL);
          }
          return;
        }
      if (g_ascii_strcasecmp (k, "Next") == 0)
        {
          const Block *b = top_block (st);

          /* Translators: Next and For are macro keywords: keep them in
           * English. */
          if (b == NULL || b->kind != BLK_FOR) { fail (st, _("Next without For")); return; }
          g_string_append (out, "next");
          pop_block (st);
          return;
        }
      if (g_ascii_strcasecmp (k, "Do") == 0)
        {
          if (from + 1 < to && is_kw (TOK (toks, from + 1), "While"))
            {
              char *c = expr_string (st, toks, from + 2, to);

              g_string_append_printf (out, "while %s", c);
              g_free (c);
              push_block (st, BLK_WHILE, NULL);
            }
          else if (from + 1 < to && is_kw (TOK (toks, from + 1), "Until"))
            {
              char *c = expr_string (st, toks, from + 2, to);

              g_string_append_printf (out, "while not (%s)", c);
              g_free (c);
              push_block (st, BLK_WHILE, NULL);
            }
          else
            {
              g_string_append (out, "do");
              push_block (st, BLK_DO, NULL);
            }
          return;
        }
      if (g_ascii_strcasecmp (k, "While") == 0)
        {
          char *c = expr_string (st, toks, from + 1, to);

          g_string_append_printf (out, "while %s", c);
          g_free (c);
          push_block (st, BLK_WHILE, NULL);
          return;
        }
      if (g_ascii_strcasecmp (k, "Wend") == 0)
        {
          const Block *b = top_block (st);

          /* Translators: Wend and While are macro keywords: keep them in
           * English. */
          if (b == NULL || b->kind != BLK_WHILE) { fail (st, _("Wend without While")); return; }
          g_string_append (out, "wend");
          pop_block (st);
          return;
        }
      if (g_ascii_strcasecmp (k, "Loop") == 0)
        {
          const Block *b = top_block (st);

          if (b == NULL || (b->kind != BLK_WHILE && b->kind != BLK_DO))
            {
              /* Translators: Loop and Do are macro keywords: keep them in
               * English. */
              fail (st, _("Loop without Do"));
              return;
            }
          if (b->kind == BLK_WHILE)
            {
              g_string_append (out, "wend");
            }
          else if (from + 1 < to && is_kw (TOK (toks, from + 1), "While"))
            {
              char *c = expr_string (st, toks, from + 2, to);

              g_string_append_printf (out, "until not (%s)", c);
              g_free (c);
            }
          else if (from + 1 < to && is_kw (TOK (toks, from + 1), "Until"))
            {
              char *c = expr_string (st, toks, from + 2, to);

              g_string_append_printf (out, "until %s", c);
              g_free (c);
            }
          else
            {
              g_string_append (out, "until false");
            }
          pop_block (st);
          return;
        }
      if (g_ascii_strcasecmp (k, "With") == 0)
        {
          guint next;
          char *chain;

          if (from + 1 >= to)
            {
              /* Translators: "With" is a macro keyword: keep it in English. */
              fail (st, _("With without an object"));
              return;
            }
          chain = read_chain (st, toks, from + 1, to, &next);
          push_block (st, BLK_WITH, chain);
          g_free (chain);
          return;
        }
      if (g_ascii_strcasecmp (k, "Dim") == 0 || g_ascii_strcasecmp (k, "ReDim") == 0)
        {
          guint start = from + 1;

          if (start < to && is_kw (TOK (toks, start), "Preserve"))
            start++;
          translate_dim (st, toks, start, to, out);
          return;
        }
      if (g_ascii_strcasecmp (k, "Const") == 0)
        {
          /* Const NAME [As T] = value. */
          guint eq = from + 1;

          while (eq < to && !is_op (TOK (toks, eq), "="))
            eq++;
          if (from + 1 < to && eq < to)
            {
              char *e = expr_string (st, toks, eq + 1, to);

              g_string_append_printf (out, "%s = %s", TOK (toks, from + 1)->text, e);
              g_free (e);
            }
          else
            /* Translators: "Const" is a macro keyword: keep it in English.
             * NAME and value stand for what is written there. */
            fail (st, _("Const expects NAME = value"));
          return;
        }
      if (g_ascii_strcasecmp (k, "Set") == 0 || g_ascii_strcasecmp (k, "Let") == 0)
        {
          translate_statement (st, toks, from + 1, to, out);
          return;
        }
      if (g_ascii_strcasecmp (k, "Call") == 0)
        {
          translate_statement (st, toks, from + 1, to, out);
          return;
        }
      if (g_ascii_strcasecmp (k, "GoTo") == 0 || g_ascii_strcasecmp (k, "GoSub") == 0 ||
          g_ascii_strcasecmp (k, "Resume") == 0)
        {
          /* Translators: "GoTo" is a macro keyword: keep it in English. */
          fail (st, _("GoTo and labels are not supported"));
          return;
        }
      if (g_ascii_strcasecmp (k, "Stop") == 0)
        {
          g_string_append (out, "end");
          return;
        }
    }

  /* A label: Name followed by a colon alone.  The colon was split off as
   * a statement separator, so a lone identifier that is not a Sub is one. */

  /* Everything else starts with a name: an assignment, a property set,
   * or a call with or without brackets. */
  if (t->kind == TOK_ID || is_op (t, "."))
    {
      guint next;
      char *chain = read_chain (st, toks, from, to, &next);
      gboolean dotted = strchr (chain, '_') != NULL;
      const Block *f = enclosing_function (st);

      if (next < to && is_op (TOK (toks, next), "="))
        {
          char *e = expr_string (st, toks, next + 1, to);

          if (dotted)
            g_string_append_printf (out, "%s_Set(%s)", chain, e);
          else if (f != NULL && g_ascii_strcasecmp (chain, f->name) == 0)
            g_string_append_printf (out, "__ret_%s = %s", f->name, e);
          else
            g_string_append_printf (out, "%s = %s", chain, e);
          g_free (e);
        }
      else if (next < to && is_op (TOK (toks, next), "("))
        {
          guint close = matching_close (toks, next, to);

          if (close + 1 < to && is_op (TOK (toks, close + 1), "="))
            {
              /* a(i) = v, or Something(x).Prop = v, which is the former. */
              char *idx = expr_string (st, toks, next + 1, close);
              char *e = expr_string (st, toks, close + 2, to);

              if (dotted)
                fail (st, _("an indexed object property cannot be set"));
              g_string_append_printf (out, "%s(%s) = %s", chain, idx, e);
              g_free (idx);
              g_free (e);
            }
          else if (close + 1 < to && !is_op (TOK (toks, close + 1), ")"))
            {
              /* Foo (a), b: the brackets were round the first argument. */
              char *args = translate_args (st, toks, next, to, chain_member (chain));

              g_string_append_printf (out, "%s%s(%s)", is_user_sub (st, chain) ? "call " : "", chain, args);
              g_free (args);
            }
          else
            {
              char *args = translate_args (st, toks, next + 1, close, chain_member (chain));

              g_string_append_printf (out, "%s%s(%s)", is_user_sub (st, chain) ? "call " : "", chain, args);
              g_free (args);
            }
        }
      else
        {
          /* A call written without brackets: MsgBox "hi", vbOKOnly. */
          char *args = translate_args (st, toks, next, to, chain_member (chain));

          g_string_append_printf (out, "%s%s(%s)", is_user_sub (st, chain) ? "call " : "", chain, args);
          g_free (args);
        }
      g_free (chain);
      return;
    }

  fail (st, _("a statement that does not start with a name"));
}

/* ---------------------------------------------------------------------- */
/* Lines                                                                   */
/* ---------------------------------------------------------------------- */

/* The source as logical lines: a line ending in " _" continues on the
 * next.  Each entry is the text and the number of physical lines it
 * covers, kept in `spans`. */
static GPtrArray *
logical_lines (const char *source, GArray *spans)
{
  GPtrArray *lines = g_ptr_array_new_with_free_func (g_free);
  char **raw = g_strsplit (source, "\n", -1);
  GString *cur = NULL;
  int span = 0;

  for (int i = 0; raw[i] != NULL; i++)
    {
      char *line = raw[i];
      gsize len = strlen (line);
      gboolean continued;

      if (len > 0 && line[len - 1] == '\r')
        line[--len] = '\0';
      while (len > 0 && g_ascii_isspace (line[len - 1]))
        line[--len] = '\0';
      continued = len >= 2 && line[len - 1] == '_' && g_ascii_isspace (line[len - 2]);
      if (continued)
        line[len - 1] = '\0';

      if (cur == NULL)
        cur = g_string_new (NULL);
      g_string_append (cur, line);
      g_string_append_c (cur, ' ');
      span++;
      if (!continued)
        {
          g_ptr_array_add (lines, g_string_free (cur, FALSE));
          g_array_append_val (spans, span);
          cur = NULL;
          span = 0;
        }
    }
  if (cur != NULL)
    {
      g_ptr_array_add (lines, g_string_free (cur, FALSE));
      g_array_append_val (spans, span);
    }
  g_strfreev (raw);
  return lines;
}

/* The first pass: the names of the Subs and Functions. */
static void
collect_subs (State *st, GPtrArray *lines)
{
  for (guint i = 0; i < lines->len; i++)
    {
      GArray *toks = tokenize (g_ptr_array_index (lines, i));
      guint k = 0;

      while (k < toks->len && (is_kw (TOK (toks, k), "Private") || is_kw (TOK (toks, k), "Public") ||
                               is_kw (TOK (toks, k), "Friend") || is_kw (TOK (toks, k), "Static")))
        k++;
      if (k + 1 < toks->len && (is_kw (TOK (toks, k), "Sub") || is_kw (TOK (toks, k), "Function")) &&
          TOK (toks, k + 1)->kind == TOK_ID)
        {
          const char *name = TOK (toks, k + 1)->text;
          char *key = g_ascii_strdown (name, -1);

          if (!g_hash_table_contains (st->subs, key))
            {
              g_hash_table_insert (st->subs, key, g_strdup (name));
              g_ptr_array_add (st->sub_names, g_strdup (name));
            }
          else
            g_free (key);
        }
      g_array_free (toks, TRUE);
    }
}

/* The constants every macro can use, on one line.  Colours are Word's
 * BGR longs, which the runtime turns round. */
static const char PRELUDE[] =
  "vbCr = chr(13) : vbLf = chr(10) : vbCrLf = chr(13) + chr(10) : vbNewLine = vbCrLf : "
  "vbTab = chr(9) : vbNullString = \"\" : "
  "vbOKOnly = 0 : vbOKCancel = 1 : vbAbortRetryIgnore = 2 : vbYesNoCancel = 3 : "
  "vbYesNo = 4 : vbRetryCancel = 5 : vbCritical = 16 : vbQuestion = 32 : "
  "vbExclamation = 48 : vbInformation = 64 : "
  "vbOK = 1 : vbCancel = 2 : vbAbort = 3 : vbRetry = 4 : vbIgnore = 5 : vbYes = 6 : vbNo = 7 : "
  "vbBlack = 0 : vbRed = 255 : vbGreen = 65280 : vbYellow = 65535 : vbBlue = 16711680 : "
  "vbMagenta = 16711935 : vbCyan = 16776960 : vbWhite = 16777215 : "
  "wdColorAutomatic = -16777216 : wdColorBlack = 0 : wdColorRed = 255 : wdColorGreen = 32768 : "
  "wdColorBlue = 16711680 : wdColorWhite = 16777215 : "
  "wdAlignParagraphLeft = 0 : wdAlignParagraphCenter = 1 : wdAlignParagraphRight = 2 : "
  "wdAlignParagraphJustify = 3 : "
  "wdCharacter = 1 : wdWord = 2 : wdSentence = 3 : wdParagraph = 4 : wdLine = 5 : wdStory = 6 : "
  "wdMove = 0 : wdExtend = 1 : wdCollapseEnd = 0 : wdCollapseStart = 1 : "
  "wdReplaceNone = 0 : wdReplaceOne = 1 : wdReplaceAll = 2 : "
  "wdFindStop = 0 : wdFindContinue = 1 : wdFindAsk = 2 : "
  "wdUnderlineNone = 0 : wdUnderlineSingle = 1 : wdUnderlineWords = 2 : wdUnderlineDouble = 3 : "
  "wdToggle = 9999998 : "
  "wdStatisticWords = 0 : wdStatisticLines = 1 : wdStatisticPages = 2 : "
  "wdStatisticCharacters = 3 : wdStatisticParagraphs = 4 : wdStatisticCharactersWithSpaces = 5 : "
  "wdDoNotSaveChanges = 0 : wdSaveChanges = -1 : wdPromptToSaveChanges = -2 : "
  "wdFormatRTF = 6 : wdFormatDocumentDefault = 16 : wdFormatXMLDocument = 12 : "
  "wdFormatOpenDocumentText = 23 : wdFormatHTML = 8 : wdFormatPDF = 17 : wdFormatText = 2 : "
  "wdStyleNormal = -1 : wdStyleHeading1 = -2 : wdStyleHeading2 = -3 : wdStyleHeading3 = -4 : "
  "wdStyleTitle = -63 : "
  "wdSectionBreakNextPage = 2 : wdSectionBreakContinuous = 3 : wdLineBreak = 6 : "
  "wdPageBreak = 7 : wdColumnBreak = 8";

W42VbaProgram *
w42_vba_translate (const char *source, const char *entry)
{
  W42VbaProgram *prog = g_new0 (W42VbaProgram, 1);
  State st;
  GArray *spans = g_array_new (FALSE, FALSE, sizeof (int));
  GPtrArray *lines;
  int physical = 1;

  g_return_val_if_fail (source != NULL, prog);

  memset (&st, 0, sizeof st);
  st.out = g_string_new (NULL);
  st.line_map = g_array_new (FALSE, FALSE, sizeof (int));
  st.subs = g_hash_table_new_full (g_str_hash, g_str_equal, g_free, g_free);
  st.sub_names = g_ptr_array_new_with_free_func (g_free);
  st.blocks = g_array_new (FALSE, FALSE, sizeof (Block));

  lines = logical_lines (source, spans);
  collect_subs (&st, lines);

  /* Row 1 is the prelude. */
  {
    int zero = 0;

    g_string_append (st.out, PRELUDE);
    g_string_append_c (st.out, '\n');
    g_array_append_val (st.line_map, zero);
  }

  for (guint i = 0; i < lines->len && st.error == NULL; i++)
    {
      GArray *toks = tokenize (g_ptr_array_index (lines, i));
      GString *line_out = g_string_new (NULL);
      guint start = 0;
      int depth = 0;

      st.source_line = physical;
      /* Statements on one line, split at the colons. */
      for (guint k = 0; k <= toks->len; k++)
        {
          gboolean end = (k == toks->len);

          if (!end)
            {
              const Tok *t = TOK (toks, k);

              if (is_op (t, "(")) depth++;
              else if (is_op (t, ")")) depth--;
              else if (depth == 0 && is_op (t, ":")) end = TRUE;
            }
          if (!end)
            continue;
          if (k > start)
            {
              GString *stmt = g_string_new (NULL);

              /* "Label:" -- a name alone before a colon -- is not ours;
               * a keyword alone before one is a statement. */
              if (k == start + 1 && k < toks->len && TOK (toks, start)->kind == TOK_ID &&
                  !is_user_sub (&st, TOK (toks, start)->text) &&
                  !is_reserved (TOK (toks, start)->text) && k == 1)
                {
                  fail (&st, _("labels are not supported"));
                }
              else
                {
                  translate_statement (&st, toks, start, k, stmt);
                  if (stmt->len > 0)
                    {
                      if (line_out->len > 0)
                        g_string_append (line_out, " : ");
                      g_string_append (line_out, stmt->str);
                    }
                }
              g_string_free (stmt, TRUE);
            }
          start = k + 1;
        }
      g_string_append (st.out, line_out->str);
      g_string_append_c (st.out, '\n');
      g_array_append_val (st.line_map, physical);
      g_string_free (line_out, TRUE);
      g_array_free (toks, TRUE);
      physical += g_array_index (spans, int, i);
    }

  if (st.error == NULL && st.blocks->len > 0)
    {
      const Block *b = top_block (&st);
      static const char *const names[] = { "Sub", "Function", "If", "For", "Do", "Do", "Select Case", "With" };

      st.source_line = physical - 1;
      {
        /* Translators: %s is a macro keyword (Sub, Function, If, For, Do,
         * Select Case or With): keep it in English. */
        char *msg = g_strdup_printf (_("%s is never ended"), names[b->kind]);

        fail (&st, msg);
        g_free (msg);
      }
    }

  if (st.error == NULL && entry != NULL)
    {
      int zero = 0;

      if (!is_user_sub (&st, entry))
        {
          st.source_line = 0;
          g_free (st.error);
          /* Translators: "Sub" is a macro keyword: keep it in English.
           * %s is the Sub's name. */
          st.error = g_strdup_printf (_("There is no Sub named %s in this macro."), entry);
        }
      else
        {
          g_string_append_printf (st.out, "call %s()\n", entry);
          g_array_append_val (st.line_map, zero);
        }
    }

  while (st.blocks->len > 0)
    pop_block (&st);

  prog->subs = st.sub_names;
  prog->line_map = st.line_map;
  prog->error = st.error;
  prog->program = st.error == NULL ? g_string_free (st.out, FALSE) : NULL;
  if (st.error != NULL)
    g_string_free (st.out, TRUE);
  g_hash_table_unref (st.subs);
  g_array_free (st.blocks, TRUE);
  g_ptr_array_unref (lines);
  g_array_free (spans, TRUE);
  return prog;
}

void
w42_vba_program_free (W42VbaProgram *program)
{
  if (program == NULL)
    return;
  g_free (program->program);
  g_free (program->error);
  if (program->subs != NULL)
    g_ptr_array_unref (program->subs);
  if (program->line_map != NULL)
    g_array_free (program->line_map, TRUE);
  g_free (program);
}

int
w42_vba_source_line (const W42VbaProgram *program, int mb_row)
{
  if (program == NULL || program->line_map == NULL || mb_row < 1 ||
      (guint) mb_row > program->line_map->len)
    return 0;
  return g_array_index (program->line_map, int, mb_row - 1);
}

char **
w42_vba_list_subs (const char *source, gboolean subs_only)
{
  W42VbaProgram *prog = w42_vba_translate (source != NULL ? source : "", NULL);
  GPtrArray *names = g_ptr_array_new ();

  (void) subs_only;
  for (guint i = 0; prog->subs != NULL && i < prog->subs->len; i++)
    g_ptr_array_add (names, g_strdup (g_ptr_array_index (prog->subs, i)));
  g_ptr_array_add (names, NULL);
  w42_vba_program_free (prog);
  return (char **) g_ptr_array_free (names, FALSE);
}
