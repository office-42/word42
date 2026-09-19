# Word42 — goals and to-do

Goals:
- Feature parity with Microsoft Word 97: the word processor of the late
  1990s that the ones since are still built on.  Word 97 is the target:
  its menus are the specification, its dialogs are the models, and
  docs/PARITY.md keeps the score against it, command by command.
- AbiWord feature parity as the second yardstick, since it is the free
  word processor closest in spirit: https://github.com/AbiWord/abiword/

What Word42 does today is in the README. This is what it does not do yet.
The menus already name some of these and show them greyed out: the menu bar
is the specification. Rough order of work; see also docs/ROADMAP.md for the
internals worth doing regardless.

## To do — what Word 97 added

See docs/PARITY.md for the full comparison with Word 97 and AbiWord.

- Text boxes placed freely on the page, and the Drawing toolbar's
  AutoShapes, WordArt and editable shapes (drawings are pictures with a
  box of their own today).
- Revisions coloured by author (one author today).
- Outline view; Master Document.
- Grammar checking; the Office Assistant is not planned.
- Insert > Index and Tables: Table of Authorities.
- File > Versions (several versions kept inside one document).
- Format > Text Direction (vertical text in table cells); Table > Draw
  Table with the pencil and eraser.
- Format > Font: the Animation tab (blinking backgrounds and marching
  ants are not planned) and Character Spacing's kerning and scale.
- AutoCorrect exceptions.

## To do — editing and layout

- Per-section page size and orientation; gutter and mirror margins.
- Headers and footers of more than one line, edited on the page.
- Nested tables.

## To do — files

- Word .doc: metafile pictures; the formatting of files from before
  Word 97 (only their text is read today). Writing .doc is probably
  never worth doing: RTF is what Word opens.

## To do — application

- Arrange All (GTK 4 cannot place windows; may never happen).
- Publish on the Windows Store (the MSIX is built with the reserved
  product's own identity and CI keeps it building; what is left is a
  published privacy policy and the submission -- docs/WINDOWS-STORE.md);
  sign and notarise the macOS .app (the CI makes one, unsigned); Flatpak
  on Flathub (the manifest is in build-aux; the module checksums need
  confirming against the releases).
- Translations; accessibility.
