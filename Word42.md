# Word42 — goals and to-do

Goals:
- AbiWord feature parity: https://github.com/AbiWord/abiword/
- Feature parity with the word processors of the early 1990s (Word 6)

What word42 does today is in the README. This is what it does not do yet.
The menus already name some of these and show them greyed out: the menu bar
is the specification. Rough order of work; see also docs/ROADMAP.md for the
internals worth doing regardless.

## To do — editing and layout

See docs/PARITY.md for the full comparison with AbiWord and Word.

- Table borders per cell side; text boxes placed freely on the page.
- Drop caps, text boxes and text frames (pictures do wrap).
- Styles that follow their base when the base changes (a new style is a
  copy of its base today).
- Drawing shapes that stay editable (they are pictures today); more
  picture formats (metafiles).

## To do — files

- Word .doc: metafile pictures; Word 6/95
  formatting (only their text is read today). Writing .doc is probably
  never worth doing: RTF is what Word opens.

## To do — application

- Arrange All (GTK 4 cannot place windows; may never happen).
- Publish on the Windows Store (the MSIX is built with the reserved
  product's own identity and CI keeps it building; what is left is a
  published privacy policy and the submission -- docs/WINDOWS-STORE.md);
  sign and notarise the macOS .app (the CI makes one, unsigned); Flatpak
  on Flathub (the manifest is in build-aux; the module checksums need
  confirming against the releases).
- Translations.
