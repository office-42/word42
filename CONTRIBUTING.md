# Contributing to word42

## Licence

word42 is GPL-3.0-or-later. By contributing you agree your work is licensed
the same way. Put an SPDX line at the top of every new file:

```c
/* SPDX-License-Identifier: GPL-3.0-or-later */
```

Do not paste in code from other projects, AbiWord included. word42 studies
AbiWord's design and shares none of its code; AbiWord is GPL-2.0, which is not
compatible with GPL-3 in that direction in any case.

## Before you send a patch

```sh
meson setup builddir --werror
meson compile -C builddir
```

Both must be clean, with no warnings.

This project does not carry a unit-test suite. Exercise your change in the
running application before sending it, and say in the commit message what you
did to check it.

## Style

The code follows GNU/GTK style, because that is what the libraries underneath
it look like:

- two-space indent, no tabs;
- return type on its own line in a function definition;
- a space before the parenthesis of a call, as in `g_free (thing)`;
- braces on their own line, indented with the block they open;
- `w42_` on public functions, `W42` on types, `W42_` on macros;
- 79 columns, where holding to it does not hurt readability.

Two conventions worth stating outright:

**Comments say why, not what.** `/* Increment i */` is noise. `/* Word puts
the caret at the start of the following line when a position falls exactly on
a wrap */` explains why the code below it looks odd, and earns its place.

**Call things what Word calls them.** A paragraph mark is a block strux, font
sizes are in half-points, measurements are in twips. Matching the vocabulary
of the domain is worth more than matching anyone's house style.

## Layering

Dependencies point one way: `ui/` may use `layout/`, which may use `model/`,
which may use nothing above it. The model does not include GTK and must stay
that way: it is the part of word42 that could be reused by anything, and the
part whose correctness is easiest to reason about when nothing graphical can
reach into it.

## Commit messages

A one-line summary in the imperative, a blank line, then prose saying why the
change is right. If the change is subtle, the commit message is where the
reasoning belongs. Someone reading `git log` should not have to reconstruct
it from the diff.
