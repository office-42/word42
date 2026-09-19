/* w42-vba.h - Word42 Basic: a dialect of Visual Basic for Applications,
 * translated into MY-BASIC's own syntax before the engine sees it
 *
 * Copyright (C) 2026 Andreas Røsdal
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * A macro is written the way Word's were -- Sub and End Sub, Dim x As
 * String, If ... End If, Do ... Loop, Selection.TypeText "hello" -- and
 * MY-BASIC wants def and enddef, endif, while and wend, and knows no
 * objects.  This translator rewrites the one into the other, line for
 * line, so that an error the engine reports can be given against the line
 * the macro was written on.  The object model's dotted names become
 * calls to native functions -- Selection.Font.Bold = True becomes
 * Selection_Font_Bold_Set(true), and Selection.Text in an expression
 * becomes Selection_Text() -- which the macro runtime registers.
 */

#pragma once

#include <glib.h>

G_BEGIN_DECLS

typedef struct {
  char      *program;    /* the MY-BASIC program; NULL when it could not be made */
  GPtrArray *subs;       /* the Subs and Functions declared, in order (char*) */
  GArray    *line_map;   /* int: MY-BASIC row (1-based) -> source line (1-based) */
  char      *error;      /* what could not be translated, with its line, or NULL */
} W42VbaProgram;

/* Translates `source`.  `entry` names the Sub to run at the end, or is
 * NULL for none: the translation is then only the declarations, for
 * listing them.  The result is never NULL; `program` is NULL and `error`
 * set when the source has something this dialect does not do. */
W42VbaProgram *w42_vba_translate (const char *source, const char *entry);
void           w42_vba_program_free (W42VbaProgram *program);

/* The line of the macro's own source that row `mb_row` of the program
 * came from, or 0 for a line the translator added. */
int            w42_vba_source_line (const W42VbaProgram *program, int mb_row);

/* The names of the Subs and Functions in `source`, for a list; free with
 * g_strfreev().  Functions are listed after Subs when `subs_only` is
 * FALSE and left out when it is TRUE. */
char         **w42_vba_list_subs (const char *source, gboolean subs_only);

G_END_DECLS
