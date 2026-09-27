# Changelog

## 1.0.6 (in development)

### Security

- **LaTeX is run sandboxed.** A document's mathematics goes to LaTeX as
  the TeX it is written in, so a document that said `$\input{...}$`
  could have LaTeX set any file the user can read into the preview or
  the exported PDF. The engine now runs with no shell escape and may only
  read and write in its own working folder: TeX Live's `openin_any` and
  `openout_any` in paranoid mode, MiKTeX's own switches for the same, and
  Tectonic's untrusted mode.
- **Web pages written by Word42 keep a font name to a name.** A font name
  with a line break in it, from a crafted RTF or HTML file, could add
  style rules of its own to an exported page -- an image fetched from
  elsewhere, or a box laid over the page.
- **A crafted file can no longer make Word42 read past the end of its
  data** in a .docx's HYPERLINK field that ends in a backslash, or in a
  .doc's bookmark table that ends halfway through a name's length.
- **Files made to be expensive open in reasonable time and memory.**
  Small .odt, .abw and .docx files whose tables made millions of empty
  cells, .odt files of repeated spaces, WordPerfect files whose notes all
  name one packet, a picture set in many places, RTF list tables and
  stylesheets, and HTML style sheets and notes built to cost the square
  of their size each took minutes or gigabytes; they are now held to a
  budget, shared, or read in one pass.
- Page numbers, tab stops, indents, column gaps and paragraph spacing
  near the limits of an integer are held to a page's range, where they
  overflowed on the way into the layout.

### Fixed

- The WordPerfect writer numbers a numbered list's items 1, 2, 3 rather
  than 1 for each.
- A .doc file's language is read, so spelling, hyphenation and LaTeX
  follow it; .doc files came back in no language at all.
- A .doc table row of more than 22 shaded cells keeps every cell's
  shading.
- A plain-text file in UTF-16 keeps what follows a NUL character, and a
  text file of many line breaks saves at once.

## 1.0.5

### New

- **LaTeX mode** (File ▸ LaTeX Mode). With it on, File ▸ Export as PDF
  and File ▸ LaTeX Preview hand the document to LaTeX, which typesets it
  in Latin Modern, the Computer Modern of theses and papers. LaTeX breaks
  the lines with Knuth's algorithm and sets ligatures, hyphenation and
  microtype. Headings become numbered or unnumbered sections, and the
  Title, Subtitle and author become the title block. Footnotes, endnotes,
  lists, tables, pictures, links, headers, footers and page numbers are
  carried over. Word42 uses Tectonic, LuaLaTeX, XeLaTeX or pdfLaTeX,
  whichever is installed. Save As also writes the LaTeX source (.tex).
- **Versions, kept in Git** (File ▸ Versions), Word 97's box: Save Now
  with a comment, a check box to keep a version each time the document
  is saved, and the versions by date, author and comment, to open,
  compare with the document or restore. Each version is a commit of the
  document's file in a Git repository in its folder -- the one already
  there, or a new one -- so `git log` and a push to a server work on them
  as on any other. It is chosen document by document, and nothing else in
  the repository is touched. Git is built in; no git program is needed.
- **LaTeX Preview** (File ▸ LaTeX Preview) is now a pane beside the page
  that sets the document again whenever the typing pauses. It keeps the
  place being written in sight, and a double-click on a page puts the
  caret at those words (SyncTeX). The LaTeX source can be shown beside,
  and when LaTeX stops, what it said is shown over the last good pages,
  with the line in the source and the paragraph in the text it came from.
- **Mathematics** in LaTeX's notation -- `$...$`, `\(...\)`, `$$...$$`,
  `\[...\]` and the amsmath environments such as `equation` and `align`
  -- is set as mathematics by LaTeX, in the preview and the PDF.

### Improved

- LaTeX: a table of contents or of figures becomes LaTeX's own, a picture
  with its caption becomes a numbered figure, bookmarks and the links to
  them work in the PDF, and unnumbered headings are in the PDF's outline.
- Export as PDF in LaTeX mode runs LaTeX again until the page references
  settle, and clears away the folder it worked in.
- `word42 --convert-to=tex` writes the LaTeX source.
- The new menus, dialogs and messages are translated into the nine
  languages Word42 speaks besides English.

### Fixed

- Enter in a dialog's number box, such as Go To's page number, presses
  the dialog's default button. Going to a page in Page Layout view shows
  the page from its top edge.

## 1.0.4

### New

- **Word 97 .doc is the default save format.** Word42 now writes .doc files
  as Word 97 did, not only reads them. They include text and formatting,
  styles, sections and columns, headers and footers with their fields,
  footnotes and endnotes, tables, lists, pictures, links, bookmarks,
  comments, tracked changes, page borders and document properties.
  Word opens them as it opens its own. Save As suggests .doc for a new
  document, and a document opened from a .doc is saved back to it.
- **WordPerfect documents.** File ▸ Open reads WordPerfect 6 and later
  (.wpd), and the WordPerfect 5.x files of DOS, even when they are named
  .doc. Save As writes WordPerfect 6.
- **Make It Fit** (Format ▸ Make It Fit), from WordPerfect: tell it how many
  pages the document should fill, and it scales the font size, line spacing
  and margins together until it does, smaller or larger.
- **Page numbers can begin on any page.** Insert ▸ Page Numbers now sets the
  page the numbering begins on and the number it starts at; the pages before
  it show no number.

### Improved

- The font-size list shows every size at once, without scrolling.
- Reading a .doc keeps its own paragraph styles, its first-page and even-page
  headers, its section breaks and columns, comments, bookmarks and fields.

### Fixed

- Choosing a font size from the toolbar no longer jumps the view to the top
  of the document.
- Footnotes and endnotes no longer swap places when an RTF file from Word42
  is opened in Word.
- A line spacing of 108% no longer comes back as 107% from a .doc.
