/* w42-mathtex.h - equations typed as LaTeX writes them, and back
 *
 * Copyright (C) 2026 Andreas Røsdal
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * Nobody types MathML.  An equation is typed in LaTeX's notation --
 * \frac{a}{b}, x^2, \sqrt{2}, \sum_{i=1}^n -- which is what mathematicians
 * already write, and made MathML here, with what was typed kept beside
 * it in the formula as an annotation, so that the equation opens to be
 * edited as it was typed.  A formula that came from a file without one
 * is written back into LaTeX's notation from its MathML, for editing and
 * for the LaTeX export.
 *
 * The notation is LaTeX's with amsmath's: the Greek letters, the
 * operators, relations and arrows, fractions, binomials, roots, scripts
 * and limits, accents, \left and \right, \big and its kin, \text and the
 * alphabets (\mathbb, \mathcal, \mathfrak, \mathbf, \mathrm...), the
 * matrix environments, cases, aligned and array, spaces, and \color.
 */

#pragma once

#include "w42-math.h"

G_BEGIN_DECLS

/* The encoding a formula's LaTeX is kept under in its <semantics>. */
#define W42_TEX_ENCODING "application/x-tex"

/* LaTeX to MathML, the LaTeX kept as the formula's annotation; set on a
 * line of its own when `display`.  NULL, with `error` saying where, when
 * the braces do not balance or an environment is not ended; a command
 * it does not know is shown as itself in red, as TeX would stop at it. */
char *w42_tex_to_mathml (const char *tex, gboolean display, GError **error);

/* The formula in LaTeX's notation: its annotation when it has one,
 * otherwise made from the MathML. */
char *w42_mathml_to_tex (const W42MathNode *root);

/* The formula as plain text, for the formats that have no mathematics:
 * x² + 1 as "x^2 + 1", a fraction as "(a)/(b)". */
char *w42_mathml_to_text (const W42MathNode *root);

G_END_DECLS
