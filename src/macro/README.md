# The macro engine

`my_basic.c` and `my_basic.h` are [MY-BASIC](https://github.com/paladin-t/my_basic)
by Tony Wang, taken unchanged from its `core/` directory and used under
the MIT licence in `LICENSE.my_basic`.  It is the interpreter behind
Tools ▸ Macro: a BASIC in one C file, with functions, arrays, strings
and a way to register native functions, which is what a word
processor's macro language needs.

Word42's macros are written in a dialect of Visual Basic for
Applications -- `Sub`, `End Sub`, `Dim x As String`, `Selection.TypeText`
-- and `w42-vba.c` translates that into MY-BASIC's own syntax before
the engine sees it: the object model's dotted names become calls to
native functions, which `src/ui/w42-macro.c` registers over the view.

The engine is compiled as its own library with warnings off, so the
project's `-Werror` build does not police a file it does not maintain.
