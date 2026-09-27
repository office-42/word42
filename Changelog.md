# Changelog

## 1.0.5 (in development)

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
