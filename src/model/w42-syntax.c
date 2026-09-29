/* w42-syntax.c - see w42-syntax.h
 *
 * Copyright (C) 2026 Andreas Røsdal
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * Scanners rather than parsers: an editor colours what it can see on the
 * line in front of it, and so do these, carrying from line to line only
 * what a line can leave open.  They are never wrong about the text -- a
 * token is only ever a stretch of it -- only now and then about what a
 * stretch is.
 */

#include "w42-syntax.h"

#include <string.h>

/* The state a paragraph leaves, packed in 32 bits:
 *
 *   bits 0-3    the markup scanner's mode (M_*)
 *   bit  4      inside an HTML <script>: its text is JavaScript
 *   bit  5      inside an HTML <style>: its text is CSS
 *   bits 6-7    the element a tag being read opens: 1 script, 2 style
 *   bits 8-11   the JavaScript or CSS scanner's mode (J_*)
 *   bit  12     a slash here would be division, not a regular expression
 *   bit  13     CSS: inside a rule's braces */
enum { M_TEXT, M_COMMENT, M_TAG, M_VALUE_DQ, M_VALUE_SQ, M_CDATA, M_PI, M_DECL };
enum { J_CODE, J_COMMENT, J_TEMPLATE };

#define M_MODE(s)      ((s) & 0xFu)
#define IN_SCRIPT      (1u << 4)
#define IN_STYLE       (1u << 5)
#define PENDING(s)     (((s) >> 6) & 3u)
#define J_MODE(s)      (((s) >> 8) & 0xFu)
#define J_NOREGEX      (1u << 12)
#define CSS_IN_RULE    (1u << 13)

#define WITH_M_MODE(s, m)   (((s) & ~0xFu) | (guint32) (m))
#define WITH_PENDING(s, p)  (((s) & ~(3u << 6)) | ((guint32) (p) << 6))
#define WITH_J_MODE(s, m)   (((s) & ~(0xFu << 8)) | ((guint32) (m) << 8))

W42SyntaxLang
w42_syntax_style_lang (W42StyleSheet *sheet, const char *style)
{
  /* A style's bases, as far as a chain of them sensibly goes. */
  for (int depth = 0; style != NULL && depth < 16; depth++)
    {
      const W42Style *s;

      if (g_str_equal (style, W42_SYNTAX_STYLE_HTML))
        return W42_SYNTAX_HTML;
      if (g_str_equal (style, W42_SYNTAX_STYLE_JAVASCRIPT))
        return W42_SYNTAX_JAVASCRIPT;
      if (g_str_equal (style, W42_SYNTAX_STYLE_XML))
        return W42_SYNTAX_XML;
      s = sheet != NULL ? w42_stylesheet_find (sheet, style) : NULL;
      style = s != NULL ? s->based_on : NULL;
    }
  return W42_SYNTAX_NONE;
}

const char *
w42_syntax_lang_style (W42SyntaxLang lang)
{
  switch (lang)
    {
    case W42_SYNTAX_HTML:       return W42_SYNTAX_STYLE_HTML;
    case W42_SYNTAX_JAVASCRIPT: return W42_SYNTAX_STYLE_JAVASCRIPT;
    case W42_SYNTAX_XML:        return W42_SYNTAX_STYLE_XML;
    default:                    return NULL;
    }
}

const char *
w42_syntax_lang_id (W42SyntaxLang lang)
{
  switch (lang)
    {
    case W42_SYNTAX_HTML:       return "html";
    case W42_SYNTAX_JAVASCRIPT: return "javascript";
    case W42_SYNTAX_XML:        return "xml";
    default:                    return NULL;
    }
}

W42SyntaxLang
w42_syntax_lang_from_id (const char *id)
{
  /* The names highlight.js and Prism know each language by. */
  static const struct { const char *id; W42SyntaxLang lang; } IDS[] = {
    { "html", W42_SYNTAX_HTML }, { "htm", W42_SYNTAX_HTML },
    { "xhtml", W42_SYNTAX_HTML },
    { "javascript", W42_SYNTAX_JAVASCRIPT }, { "js", W42_SYNTAX_JAVASCRIPT },
    { "mjs", W42_SYNTAX_JAVASCRIPT }, { "cjs", W42_SYNTAX_JAVASCRIPT },
    { "jsx", W42_SYNTAX_JAVASCRIPT }, { "ecmascript", W42_SYNTAX_JAVASCRIPT },
    { "xml", W42_SYNTAX_XML }, { "svg", W42_SYNTAX_XML },
    { "xsl", W42_SYNTAX_XML }, { "xslt", W42_SYNTAX_XML },
    { "xsd", W42_SYNTAX_XML }, { "rss", W42_SYNTAX_XML },
    { "atom", W42_SYNTAX_XML }, { "plist", W42_SYNTAX_XML },
  };

  if (id == NULL)
    return W42_SYNTAX_NONE;
  for (guint i = 0; i < G_N_ELEMENTS (IDS); i++)
    if (g_ascii_strcasecmp (id, IDS[i].id) == 0)
      return IDS[i].lang;
  return W42_SYNTAX_NONE;
}

void
w42_syntax_look (W42SyntaxKind kind, guint32 *rgb, gboolean *italic)
{
  /* The colours an editor on a white page uses, near enough. */
  static const guint32 COLOURS[W42_SYNTAX_KINDS] = {
    [W42_SYNTAX_PLAIN]     = 0x000000,
    [W42_SYNTAX_KEYWORD]   = 0x0000FF,
    [W42_SYNTAX_BUILTIN]   = 0x267F99,
    [W42_SYNTAX_STRING]    = 0xA31515,
    [W42_SYNTAX_NUMBER]    = 0x098658,
    [W42_SYNTAX_COMMENT]   = 0x008000,
    [W42_SYNTAX_REGEX]     = 0x811F3F,
    [W42_SYNTAX_TAG]       = 0x800000,
    [W42_SYNTAX_ATTRIBUTE] = 0xE50000,
    [W42_SYNTAX_VALUE]     = 0x0000FF,
    [W42_SYNTAX_ENTITY]    = 0x800080,
    [W42_SYNTAX_PROLOG]    = 0x808080,
  };

  if (rgb != NULL)
    *rgb = (guint) kind < W42_SYNTAX_KINDS ? COLOURS[kind] : 0;
  if (italic != NULL)
    *italic = kind == W42_SYNTAX_COMMENT;
}

/* ---- tokens --------------------------------------------------------- */

static void
add_token (GArray *tokens, gsize start, gsize end, W42SyntaxKind kind)
{
  W42SyntaxToken token;

  if (tokens == NULL || end <= start || kind == W42_SYNTAX_PLAIN)
    return;
  if (tokens->len > 0)
    {
      W42SyntaxToken *last = &g_array_index (tokens, W42SyntaxToken, tokens->len - 1);

      if (last->kind == kind && last->end == start)
        {
          last->end = end;
          return;
        }
    }
  token.start = start;
  token.end = end;
  token.kind = kind;
  g_array_append_val (tokens, token);
}

/* Where `what` next begins in [from, to), or `to`. */
static gsize
find (const char *s, gsize from, gsize to, const char *what)
{
  const char *hit;

  if (from >= to)
    return to;
  hit = g_strstr_len (s + from, (gssize) (to - from), what);
  return hit != NULL ? (gsize) (hit - s) : to;
}

/* The same without regard to ASCII case: </SCRIPT> ends a script too. */
static gsize
find_nocase (const char *s, gsize from, gsize to, const char *what)
{
  gsize n = strlen (what);

  for (gsize i = from; i + n <= to; i++)
    if (g_ascii_strncasecmp (s + i, what, n) == 0)
      return i;
  return to;
}

static gboolean
in_list (const char *s, gsize n, const char *const *list)
{
  for (guint i = 0; list[i] != NULL; i++)
    if (strlen (list[i]) == n && memcmp (list[i], s, n) == 0)
      return TRUE;
  return FALSE;
}

/* A byte that can go on in a name: letters, digits, and every byte of a
 * character past ASCII, which is a letter as far as a name goes. */
static inline gboolean
name_byte (guchar c)
{
  return g_ascii_isalnum (c) || c == '_' || c == '$' || c >= 0x80;
}

/* ---- JavaScript ------------------------------------------------------ */

static const char *const JS_KEYWORDS[] = {
  "async", "await", "break", "case", "catch", "class", "const", "continue",
  "debugger", "default", "delete", "do", "else", "export", "extends", "false",
  "finally", "for", "function", "if", "import", "in", "instanceof", "let",
  "new", "null", "of", "return", "static", "super", "switch", "this", "throw",
  "true", "try", "typeof", "undefined", "var", "void", "while", "with",
  "yield", "NaN", "Infinity", NULL
};

/* After these a slash starts a regular expression, as after an operator. */
static const char *const JS_BEFORE_REGEX[] = {
  "return", "typeof", "case", "do", "else", "in", "of", "new", "delete",
  "void", "throw", "instanceof", "yield", "await", NULL
};

static const char *const JS_BUILTINS[] = {
  "Array", "BigInt", "Boolean", "console", "Date", "document", "Error",
  "globalThis", "Intl", "isNaN", "JSON", "Map", "Math", "Number", "Object",
  "parseFloat", "parseInt", "Promise", "Proxy", "Reflect", "RegExp",
  "require", "Set", "String", "Symbol", "WeakMap", "WeakSet", "window", NULL
};

/* JavaScript from `i` to `end`: in HTML, the text of a <script>. */
static guint32
scan_js (const char *s, gsize i, gsize end, guint32 state, GArray *tokens)
{
  guint mode = J_MODE (state);
  gboolean regex_ok = (state & J_NOREGEX) == 0;
  gsize open_at = i;                  /* where the comment or template began */
  gsize search = i;                   /* where to look for its end from */

  while (i < end)
    {
      guchar c;

      if (mode == J_COMMENT)
        {
          gsize close = find (s, search, end, "*/");

          if (close < end)
            {
              add_token (tokens, open_at, close + 2, W42_SYNTAX_COMMENT);
              i = close + 2;
              mode = J_CODE;
            }
          else
            {
              add_token (tokens, open_at, end, W42_SYNTAX_COMMENT);
              i = end;
            }
          continue;
        }
      if (mode == J_TEMPLATE)
        {
          gsize j = search;

          while (j < end && s[j] != '`')
            j += s[j] == '\\' && j + 1 < end ? 2 : 1;
          if (j < end)
            {
              add_token (tokens, open_at, j + 1, W42_SYNTAX_STRING);
              i = j + 1;
              mode = J_CODE;
              regex_ok = FALSE;
            }
          else
            {
              add_token (tokens, open_at, end, W42_SYNTAX_STRING);
              i = end;
            }
          continue;
        }

      c = (guchar) s[i];
      if (c == ' ' || c == '\t')
        {
          i++;
          continue;
        }
      if (c == '/' && i + 1 < end && s[i + 1] == '/')
        {
          add_token (tokens, i, end, W42_SYNTAX_COMMENT);
          i = end;
          continue;
        }
      if (c == '/' && i + 1 < end && s[i + 1] == '*')
        {
          mode = J_COMMENT;
          open_at = i;
          search = i + 2;
          continue;
        }
      if (c == '`')
        {
          mode = J_TEMPLATE;
          open_at = i;
          search = i + 1;
          continue;
        }
      if (c == '"' || c == '\'')
        {
          gsize j = i + 1;

          while (j < end && (guchar) s[j] != c)
            j += s[j] == '\\' && j + 1 < end ? 2 : 1;
          j = MIN (j + 1, end);
          add_token (tokens, i, j, W42_SYNTAX_STRING);
          i = j;
          regex_ok = FALSE;
          continue;
        }
      if (g_ascii_isdigit (c) || (c == '.' && i + 1 < end && g_ascii_isdigit (s[i + 1])))
        {
          gboolean hex = c == '0' && i + 1 < end && (s[i + 1] == 'x' || s[i + 1] == 'X');
          gsize j = i + 1;

          while (j < end && (g_ascii_isalnum (s[j]) || s[j] == '_' || s[j] == '.' ||
                             (!hex && (s[j] == '+' || s[j] == '-') &&
                              (s[j - 1] == 'e' || s[j - 1] == 'E'))))
            j++;
          add_token (tokens, i, j, W42_SYNTAX_NUMBER);
          i = j;
          regex_ok = FALSE;
          continue;
        }
      if (name_byte (c) && !g_ascii_isdigit (c))
        {
          gsize j = i + 1;

          while (j < end && name_byte ((guchar) s[j]))
            j++;
          /* A property's name, after a dot, is a name and nothing more:
           * obj.default, style.float. */
          if (i == 0 || s[i - 1] != '.')
            {
              if (in_list (s + i, j - i, JS_KEYWORDS))
                add_token (tokens, i, j, W42_SYNTAX_KEYWORD);
              else if (in_list (s + i, j - i, JS_BUILTINS))
                add_token (tokens, i, j, W42_SYNTAX_BUILTIN);
            }
          regex_ok = in_list (s + i, j - i, JS_BEFORE_REGEX);
          i = j;
          continue;
        }
      if (c == '/' && regex_ok)
        {
          /* A regular expression, to the slash that ends it outside a
           * class, and its flags; with no end on the line it was a
           * division after all. */
          gsize j = i + 1;
          gboolean in_class = FALSE;

          while (j < end && (in_class || s[j] != '/'))
            {
              if (s[j] == '\\' && j + 1 < end)
                j++;
              else if (s[j] == '[')
                in_class = TRUE;
              else if (s[j] == ']')
                in_class = FALSE;
              j++;
            }
          if (j < end && j > i + 1)
            {
              j++;
              while (j < end && g_ascii_isalpha (s[j]))
                j++;
              add_token (tokens, i, j, W42_SYNTAX_REGEX);
              i = j;
              regex_ok = FALSE;
              continue;
            }
        }
      regex_ok = !(c == ')' || c == ']');
      i++;
    }

  state = WITH_J_MODE (state, mode);
  return regex_ok ? state & ~J_NOREGEX : state | J_NOREGEX;
}

/* ---- CSS, in an HTML <style> ---------------------------------------- */

/* Comments, strings and numbers; a rule's selector as the tags it names,
 * and a property's name as an attribute's. */
static guint32
scan_css (const char *s, gsize i, gsize end, guint32 state, GArray *tokens)
{
  guint mode = J_MODE (state);
  gboolean in_rule = (state & CSS_IN_RULE) != 0;
  gsize open_at = i, search = i;

  while (i < end)
    {
      guchar c;

      if (mode == J_COMMENT)
        {
          gsize close = find (s, search, end, "*/");

          add_token (tokens, open_at, close < end ? close + 2 : end, W42_SYNTAX_COMMENT);
          i = close < end ? close + 2 : end;
          if (close < end)
            mode = J_CODE;
          continue;
        }
      c = (guchar) s[i];
      if (c == '/' && i + 1 < end && s[i + 1] == '*')
        {
          mode = J_COMMENT;
          open_at = i;
          search = i + 2;
          continue;
        }
      if (c == '"' || c == '\'')
        {
          gsize j = i + 1;

          while (j < end && (guchar) s[j] != c)
            j += s[j] == '\\' && j + 1 < end ? 2 : 1;
          j = MIN (j + 1, end);
          add_token (tokens, i, j, W42_SYNTAX_STRING);
          i = j;
          continue;
        }
      if (c == '{' || c == '}')
        {
          in_rule = c == '{';
          i++;
          continue;
        }
      if (c == '@')
        {
          gsize j = i + 1;

          while (j < end && (g_ascii_isalnum (s[j]) || s[j] == '-'))
            j++;
          add_token (tokens, i, j, W42_SYNTAX_KEYWORD);
          i = j;
          continue;
        }
      if (in_rule && (g_ascii_isdigit (c) || c == '#' ||
                      (c == '.' && i + 1 < end && g_ascii_isdigit (s[i + 1]))))
        {
          gsize j = i + 1;

          while (j < end && (g_ascii_isalnum (s[j]) || s[j] == '.' || s[j] == '%'))
            j++;
          add_token (tokens, i, j, W42_SYNTAX_NUMBER);
          i = j;
          continue;
        }
      if (name_byte (c) || c == '-')
        {
          gsize j = i + 1, k;

          while (j < end && (name_byte ((guchar) s[j]) || s[j] == '-'))
            j++;
          k = j;
          while (k < end && (s[k] == ' ' || s[k] == '\t'))
            k++;
          if (in_rule && k < end && s[k] == ':')
            add_token (tokens, i, j, W42_SYNTAX_ATTRIBUTE);
          else if (!in_rule)
            add_token (tokens, i, j, W42_SYNTAX_TAG);
          i = j;
          continue;
        }
      i++;
    }

  state = WITH_J_MODE (state, mode);
  return in_rule ? state | CSS_IN_RULE : state & ~CSS_IN_RULE;
}

/* ---- HTML and XML ----------------------------------------------------- */

static inline gboolean
markup_name_byte (guchar c)
{
  return g_ascii_isalnum (c) || c == '_' || c == '-' || c == '.' || c == ':' || c >= 0x80;
}

/* A delimited stretch that opened at `open_at` -- a comment, a
 * processing instruction, a declaration -- to the `close` that ends it,
 * looked for from `search`.  Returns where the scan goes on, and whether
 * it closed. */
static gsize
delimited (const char *s, gsize open_at, gsize search, gsize len, const char *close,
           W42SyntaxKind kind, GArray *tokens, gboolean *closed)
{
  gsize at = find (s, search, len, close);

  *closed = at < len;
  if (*closed)
    at += strlen (close);
  add_token (tokens, open_at, at, kind);
  return at;
}

static guint32
scan_markup (gboolean html, const char *s, gsize i, gsize len, guint32 state,
             GArray *tokens)
{

  while (i < len)
    {
      guint mode = M_MODE (state);
      gboolean closed = FALSE;

      /* The text of a script or a style sheet, to the tag that ends it. */
      if (html && (state & (IN_SCRIPT | IN_STYLE)))
        {
          gboolean script = (state & IN_SCRIPT) != 0;
          gsize stop = find_nocase (s, i, len, script ? "</script" : "</style");

          state = script ? scan_js (s, i, stop, state, tokens)
                         : scan_css (s, i, stop, state, tokens);
          i = stop;
          if (stop < len)
            state &= ~(IN_SCRIPT | IN_STYLE | (0xFu << 8) | J_NOREGEX | CSS_IN_RULE);
          continue;
        }

      switch (mode)
        {
        case M_COMMENT:
          i = delimited (s, i, i, len, "-->", W42_SYNTAX_COMMENT, tokens, &closed);
          break;
        case M_PI:
          i = delimited (s, i, i, len, "?>", W42_SYNTAX_PROLOG, tokens, &closed);
          break;
        case M_DECL:
          i = delimited (s, i, i, len, ">", W42_SYNTAX_PROLOG, tokens, &closed);
          break;
        case M_CDATA:
          {
            gsize at = find (s, i, len, "]]>");

            closed = at < len;
            if (closed)
              add_token (tokens, at, at + 3, W42_SYNTAX_PROLOG);
            i = closed ? at + 3 : len;
          }
          break;
        case M_VALUE_DQ:
        case M_VALUE_SQ:
          {
            gsize at = find (s, i, len, mode == M_VALUE_DQ ? "\"" : "'");

            closed = at < len;
            add_token (tokens, i, closed ? at + 1 : len, W42_SYNTAX_VALUE);
            i = closed ? at + 1 : len;
            if (closed)
              state = WITH_M_MODE (state, M_TAG);
            continue;
          }
        case M_TAG:
          {
            guchar c = (guchar) s[i];

            if (c == ' ' || c == '\t' || c == '=')
              {
                i++;
                continue;
              }
            if (c == '>' || (c == '/' && i + 1 < len && s[i + 1] == '>'))
              {
                gsize n = c == '>' ? 1 : 2;
                guint pending = PENDING (state);

                add_token (tokens, i, i + n, W42_SYNTAX_TAG);
                i += n;
                state = WITH_PENDING (WITH_M_MODE (state, M_TEXT), 0);
                if (html && n == 1 && pending == 1)
                  state |= IN_SCRIPT;
                else if (html && n == 1 && pending == 2)
                  state |= IN_STYLE;
                continue;
              }
            if (c == '"' || c == '\'')
              {
                state = WITH_M_MODE (state, c == '"' ? M_VALUE_DQ : M_VALUE_SQ);
                add_token (tokens, i, i + 1, W42_SYNTAX_VALUE);
                i++;
                continue;
              }
            {
              gsize j = i;

              while (j < len && !g_ascii_isspace (s[j]) && s[j] != '=' && s[j] != '>' &&
                     s[j] != '"' && s[j] != '\'' && !(s[j] == '/' && j + 1 < len && s[j + 1] == '>'))
                j++;
              if (j == i)
                j = i + 1;
              /* An unquoted value comes after an equals sign. */
              {
                gsize k = i;

                while (k > 0 && (s[k - 1] == ' ' || s[k - 1] == '\t'))
                  k--;
                add_token (tokens, i, j, k > 0 && s[k - 1] == '=' ? W42_SYNTAX_VALUE
                                                                  : W42_SYNTAX_ATTRIBUTE);
              }
              i = j;
            }
            continue;
          }
        default:                      /* M_TEXT */
          {
            gsize lt = find (s, i, len, "<"), amp = find (s, i, len, "&");

            if (amp < lt)
              {
                gsize j = amp + 1;

                while (j < len && j - amp < 32 && (g_ascii_isalnum (s[j]) || s[j] == '#'))
                  j++;
                if (j < len && s[j] == ';' && j > amp + 1)
                  {
                    add_token (tokens, amp, j + 1, W42_SYNTAX_ENTITY);
                    i = j + 1;
                  }
                else
                  i = amp + 1;
                continue;
              }
            if (lt >= len)
              {
                i = len;
                continue;
              }
            i = lt;
            if (len - i >= 4 && strncmp (s + i, "<!--", 4) == 0)
              {
                i = delimited (s, i, i + 4, len, "-->", W42_SYNTAX_COMMENT, tokens, &closed);
                if (!closed)
                  state = WITH_M_MODE (state, M_COMMENT);
                continue;
              }
            if (len - i >= 9 && strncmp (s + i, "<![CDATA[", 9) == 0)
              {
                gsize at;

                add_token (tokens, i, i + 9, W42_SYNTAX_PROLOG);
                at = find (s, i + 9, len, "]]>");
                if (at < len)
                  {
                    add_token (tokens, at, at + 3, W42_SYNTAX_PROLOG);
                    i = at + 3;
                  }
                else
                  {
                    i = len;
                    state = WITH_M_MODE (state, M_CDATA);
                  }
                continue;
              }
            if (i + 1 < len && (s[i + 1] == '?' || s[i + 1] == '!'))
              {
                gboolean pi = s[i + 1] == '?';

                i = delimited (s, i, i + 2, len, pi ? "?>" : ">", W42_SYNTAX_PROLOG,
                               tokens, &closed);
                if (!closed)
                  state = WITH_M_MODE (state, pi ? M_PI : M_DECL);
                continue;
              }
            {
              gsize name = i + 1 + (i + 1 < len && s[i + 1] == '/' ? 1 : 0);
              gsize j = name;

              while (j < len && markup_name_byte ((guchar) s[j]))
                j++;
              if (j == name || !(g_ascii_isalpha (s[name]) || s[name] == '_' ||
                                 (guchar) s[name] >= 0x80))
                {
                  i++;                /* a less-than sign, no tag */
                  continue;
                }
              add_token (tokens, i, j, W42_SYNTAX_TAG);
              state = WITH_M_MODE (state, M_TAG);
              /* A script's or a style sheet's text, after its start tag. */
              if (html && name == i + 1 && j - name == 6 && g_ascii_strncasecmp (s + name, "script", 6) == 0)
                state = WITH_PENDING (state, 1);
              else if (html && name == i + 1 && j - name == 5 && g_ascii_strncasecmp (s + name, "style", 5) == 0)
                state = WITH_PENDING (state, 2);
              else
                state = WITH_PENDING (state, 0);
              i = j;
            }
            continue;
          }
        }
      if (closed)
        state = WITH_M_MODE (state, M_TEXT);
    }
  return state;
}

guint32
w42_syntax_scan (W42SyntaxLang lang, const char *text, gsize len,
                 guint32 state, GArray *tokens)
{
  gsize from = 0;

  g_return_val_if_fail (text != NULL || len == 0, state);

  if (lang == W42_SYNTAX_NONE)
    return state;

  /* A line break in the paragraph (U+2028, what Shift+Enter puts in and
   * a <pre> read from a web page is made of) ends a line of the code as
   * the paragraph's end does: a // comment goes no further. */
  while (from <= len)
    {
      const char *brk = from < len ? g_strstr_len (text + from, (gssize) (len - from),
                                                   "\342\200\250") : NULL;
      gsize to = brk != NULL ? (gsize) (brk - text) : len;

      if (lang == W42_SYNTAX_JAVASCRIPT)
        state = scan_js (text, from, to, state, tokens);
      else
        state = scan_markup (lang == W42_SYNTAX_HTML, text, from, to, state, tokens);
      if (brk == NULL)
        break;
      from = to + 3;
    }
  return state;
}
