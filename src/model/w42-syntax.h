/* w42-syntax.h - colouring the source code a document quotes
 *
 * Copyright (C) 2026 Andreas Røsdal
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * A paragraph in one of the source styles -- HTML Source, JavaScript
 * Source, XML Source, or a style based on one -- is code, and the layout
 * colours its tags, keywords, strings and comments as an editor would,
 * as it is typed.  The colours are not formatting: nothing in the
 * document changes, the style is all a file keeps, and the next program
 * to open it sees quoted code in a typewriter face.
 *
 * Code runs from one paragraph to the next -- a comment, a string, a tag
 * left open at a line's end -- so the scanner is handed the state the
 * paragraph before left and hands back the state this one leaves.  A
 * state is a number, which the layout keeps in a paragraph's signature.
 */

#pragma once

#include "w42-style.h"

G_BEGIN_DECLS

typedef enum {
  W42_SYNTAX_NONE = 0,
  W42_SYNTAX_HTML,
  W42_SYNTAX_JAVASCRIPT,
  W42_SYNTAX_XML
} W42SyntaxLang;

/* The styles' names, as they are kept in files: not translated. */
#define W42_SYNTAX_STYLE_HTML       "HTML Source"
#define W42_SYNTAX_STYLE_JAVASCRIPT "JavaScript Source"
#define W42_SYNTAX_STYLE_XML        "XML Source"

typedef enum {
  W42_SYNTAX_PLAIN = 0,
  W42_SYNTAX_KEYWORD,     /* if, function, return; true, null, this */
  W42_SYNTAX_BUILTIN,     /* console, Math, document, Promise */
  W42_SYNTAX_STRING,
  W42_SYNTAX_NUMBER,
  W42_SYNTAX_COMMENT,
  W42_SYNTAX_REGEX,       /* a regular expression written as one */
  W42_SYNTAX_TAG,         /* <p>, </div>, /> */
  W42_SYNTAX_ATTRIBUTE,   /* an attribute's name */
  W42_SYNTAX_VALUE,       /* and its value */
  W42_SYNTAX_ENTITY,      /* &amp; */
  W42_SYNTAX_PROLOG,      /* <?xml ... ?>, <!DOCTYPE>, <![CDATA[ */
  W42_SYNTAX_KINDS
} W42SyntaxKind;

/* A stretch of a paragraph's text, in bytes, and what it is. */
typedef struct {
  gsize         start;
  gsize         end;
  W42SyntaxKind kind;
} W42SyntaxToken;

/* The code a paragraph in `style` is, following the style's bases: a
 * style based on JavaScript Source is JavaScript too. */
W42SyntaxLang w42_syntax_style_lang (W42StyleSheet *sheet, const char *style);

/* Scans `len` bytes of `text` as `lang`, starting in `state` (0 at the
 * top of the code), and returns the state it ends in.  With `tokens`,
 * appends a W42SyntaxToken for each stretch that is not plain, in order. */
guint32 w42_syntax_scan (W42SyntaxLang lang, const char *text, gsize len,
                         guint32 state, GArray *tokens);

/* The style a language's code is set in; NULL for none. */
const char   *w42_syntax_lang_style (W42SyntaxLang lang);

/* The language's name as the web's code highlighters write it in a
 * class, "language-html": html, javascript or xml; NULL for none. */
const char   *w42_syntax_lang_id (W42SyntaxLang lang);
/* And back, from one of those names or another the web uses for the
 * same language -- js, xhtml, svg -- in any case; NONE for another. */
W42SyntaxLang w42_syntax_lang_from_id (const char *id);

/* How a kind is shown: its colour, 0x00RRGGBB, and whether it slants. */
void w42_syntax_look (W42SyntaxKind kind, guint32 *rgb, gboolean *italic);

G_END_DECLS
