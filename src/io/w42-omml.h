/* w42-omml.h - Word's equations, Office Math, to MathML and back
 *
 * Copyright (C) 2026 Andreas Røsdal
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * Word 2007 and after keep an equation in the text as Office Math Markup
 * (m:oMath): fractions, scripts, radicals, n-ary operators, delimiters,
 * accents, matrices, each an element of its own, as MathML has, but
 * different ones.  An equation Word42 reads from a .docx is made MathML
 * here, and one it writes goes back out as Office Math, so that Word
 * opens it as an equation it can edit.
 */

#pragma once

#include <glib.h>

G_BEGIN_DECLS

#define W42_OMML_NS "http://schemas.openxmlformats.org/officeDocument/2006/math"

/* The MathML for an m:oMath element, `xml` being the element as the file
 * has it, prefixes and all; set on a line of its own when `display`.
 * `size`, when not NULL, receives the half-points its runs are set in, or
 * 0 when they do not say.  NULL for what is not Office Math. */
char *w42_omml_to_mathml (const char *xml, gsize len, gboolean display, int *size);

/* A formula in MathML as an m:oMath element, its namespace declared on
 * it, appended to `out`; in an m:oMathPara when `para`.  FALSE, and
 * nothing appended, for what is not MathML. */
gboolean w42_omml_from_mathml (GString *out, const char *mathml, gboolean para);

G_END_DECLS
