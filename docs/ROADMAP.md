# Roadmap

What Word42 does today is in the README; how it measures against AbiWord
and Word is in docs/PARITY.md.  This is what it does not do yet, in
roughly the order the work makes sense.

The menus already name some of these and show them greyed out.  That is
deliberate: the menu bar is the specification.

## Next

**Headers and footers edited in place**, with pictures and several
paragraphs, different on the first page and on odd and even pages.
Then: vertical cell merges and nested tables; incremental reflow.
Done since: Options > User Info names the author of annotations and
revisions in the files; table borders, row heights, header rows and
a cell's own borders are undo steps.

Styles follow their base now: a style keeps only the settings that
are its own, and when its base changes it and everything built on it
are recomputed and their paragraphs restyled, as one undo step; Word
files' styles inherit as Word meant them to.

Borders per cell side are done: Table Properties sets which sides of
the caret's cell are ruled, over the table's own setting, and Word,
RTF and OpenDocument carry them.

Text frames and drop caps are done: Format ▸ Frame sets a paragraph at
the left or right of the column with the text after it running down the
other side (framed paragraphs one after another share the frame), and
Format ▸ Drop Cap drops a paragraph's first letter over a number of
lines; the text beside a picture, frame or dropped letter is set line by
line now, the rest of the paragraph at full width.  Character styles and styles of the document's own are done: Format ▸
Style makes and deletes them, a character style goes on a selection,
and Word, OpenDocument and AbiWord files carry them with what they were
based on.  Split cells, row height and header rows repeated on every page are
done (Table menu, Table Properties); borders per cell side are not.
Wrapped pictures are done: a picture sits at the left or right of its
paragraph and the paragraphs beside it are set in the rest of the column
(Format ▸ Picture).  What is left of frames is text in a frame, and drop
caps, which are a frame holding one letter.

Tab leaders are done: a stop can be filled with dots, dashes or a rule,
the Tabs dialog chooses which, and every format that has a way to say it
carries it.

## After that

- Text boxes placed anywhere on the page, and frames with borders.
- Pictures wrapped top-and-bottom, or set behind the text.
- Word .doc: metafile pictures; Word 6/95 formatting (only their text
  is read today).  Writing .doc is probably never worth doing.
- Translations (gettext; the menus are already marked translatable).
- Accessibility: the drawn canvas exposes no text to screen readers.
- Publishing: Windows Store (MSIX), a signed macOS .app, Flathub.

Slides are done: View ▸ Slide Show presents the document's outline, and
`.pptx` is read and written as an outline of titles and lines.  What is
left of a presentation program -- pictures and shapes placed on a slide,
speaker's notes, transitions, a slide sorter -- is not planned: this is a
word processor that can give a talk, not a presentation program.

## Not planned

**Macros.**  Word 6 had WordBasic.  Word42 will not have a macro
language.

## Internals worth doing regardless

- **Incremental layout.**  `w42_layout_build()` reformats the whole
  document on every keystroke.  The interface does not need to change;
  only the blocks whose text moved, and the pages after the first one
  whose height changed, need redoing.
- **Position lookup.**  `pt_find()` now remembers the last piece it
  found, which covers the caret and the readers; a skip list would cover
  the rest.  Several walks are still one lookup per character
  (`next_pos`/`prev_pos`, revisions, hyphenation) and could take a piece
  at a time.
- **Table rows across pages.**  Done: a row that cannot fit a page is
  broken between its lines, the header rows repeat at the top of each
  page it runs on to, and no rule is drawn where the row was cut.  What
  is left is Word's switch for it -- "allow row to break across pages" --
  so that a short row can be forced to stay whole or to break.
- **Crash recovery, the cheap way.**  Done as an autosaved RTF copy
  every two minutes.  The piece table would allow better -- the buffers
  are append-only, so a snapshot of the piece list plus the change
  buffer could be written after every keystroke.
