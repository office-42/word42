# Changelog

## 1.0.10 (in development)

## 1.0.9

### New

- **Tables in tables.** Table ▸ Insert ▸ Table with the caret in a cell
  puts a table in the cell, as wide as the cell, and a table can go in a
  cell of that one in turn, sixteen deep. They are laid out with the row
  they are in, break across pages between their rows with it, and every
  table command works on the innermost table the caret is in. Word's
  .doc and .docx, OpenDocument, RTF, HTML and AbiWord files keep them both
  ways; LaTeX sets them as tabulars in the cell, and WordPerfect, which
  has none, gets their text as the cell's paragraphs.
- **Regular expressions in Find and Replace.** Use Regular Expressions
  looks for a pattern -- `\d+`, `colou?r`, `^Chapter \w+` -- within a
  paragraph, and in the replacement `\0` puts back what was found and
  `\1` to `\9` what the pattern's groups matched, so `(\w+), (\w+)`
  replaced with `\2 \1` turns "Twain, Mark" into "Mark Twain". A pattern
  that is not one says what is wrong with it.
- **Equations.** Insert ▸ Equation puts an equation in the text, typed
  in LaTeX's notation -- `\frac{a}{b}`, `x^2`, `\sqrt{2}`,
  `\sum_{i=1}^n`, `\begin{pmatrix}` -- or as MathML, with a preview
  that sets it as it is typed. It sits on the baseline at the size of the
  text round it and is drawn as type, so it is sharp at any zoom, on
  paper and in PDF; Display sets it as on a line of its own, and a
  double-click opens it again. OpenDocument keeps it as a LibreOffice
  Math formula, .docx as Word's own Office Math, HTML and EPUB as MathML
  and AbiWord as its equation, and each is read back from those files,
  Word's and LibreOffice's included; LaTeX gets the LaTeX, and RTF,
  .doc, text and WordPerfect a picture or the text of it.
- **Code in colour.** Three new styles, HTML Source, JavaScript Source
  and XML Source, set code in Courier New and colour it as you type, as a
  programmer's editor does: tags, attributes and values, keywords,
  strings, numbers, comments, regular expressions and entities, with the
  `<script>` and `<style>` of an HTML page coloured as JavaScript and CSS.
  A comment left open at the end of one paragraph carries on into the
  next. AutoCorrect and the spelling checker leave code alone, and HTML
  writes and reads it as `<pre class="language-...">`, so Markdown's code
  blocks come in coloured.

## 1.0.8

### Fixed

- On Windows, a maximized window no longer shrinks when a menu is opened
  with the mouse.

## 1.0.7

### Fixed

- On Windows, a window restored from the maximized size it opens at can
  be made smaller again; it stayed the size of the screen.

## 1.0.6

### New

- **PDFs are read, edited and saved back as PDFs.** Every PDF Word42
  saves carries the document inside it, as an OpenDocument file attached
  to it, so that opening the PDF again gives back the document as it was
  -- styles, tables, notes, fields -- and File ▸ Save writes it back.
  Save As a PDF makes one; a PDF from another program asks before it is
  first saved over, since its pages are laid out again.
- **PDF passwords.** File ▸ PDF Options sets Word 97's two: a password to
  open, which encrypts the PDF with AES-256 (PDF 2.0's), and a password to
  modify, without which a reader allows printing and copying and no
  changes. Opening a PDF asks for its password, and offers Read Only for
  one reserved by a password to modify, as Word 97 did.
- **Signed PDFs.** PDF Options signs the PDF with the certificate in a
  PKCS #12 file (.p12, .pfx): a PAdES signature, CMS with SHA-256 over
  the whole file, with a reason, a place and a contact.
- **Smaller PDFs.** Every PDF is packed into compressed object streams,
  with a picture used twice written once, and PDF Options can scale the
  pictures down to Print (220 ppi), Screen (150 ppi) or E-mail (96 ppi):
  the *Morild* sample goes from 2.2 MB to 0.5 MB.
- `--convert-to=pdf` takes the same as options: `--pdf-password`,
  `--pdf-modify-password`, `--pdf-sign` and its `--pdf-sign-password`,
  `--pdf-pictures`, `--pdf-uncompressed`, `--pdf-keep-document`, and
  `--password` to open a protected PDF. A password can be given as
  `env:NAME` or `file:PATH`, out of sight of the other users.

### Improved

- **The title bar is the system's own**, in place of the navy one Word42
  drew, on Windows, macOS and Linux alike. It moves, maximizes and snaps
  the window as every other program's does -- on Windows to the top,
  sides and corners of every screen, with Snap Layouts and the window
  menu -- and takes the system's colours and dark mode. On Windows the
  dialogs have Windows' title bar too, where GTK drew one of its own.
- A PDF from another program comes in closer to what it was: paragraphs
  with their alignment, indents and space above; its bookmarks as
  headings; its web links; the pictures where they stood between the
  paragraphs; the header and footer on every page as the document's own,
  the page number a field; a page break for each page; its title and
  author as the Summary Info; and right-to-left text in reading order.

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

- A paragraph of a PDF whose first line was indented came in as two, its
  first line and the rest; and a page with right-to-left text on it came
  in as bare lines, without its fonts.
- The WordPerfect writer numbers a numbered list's items 1, 2, 3 rather
  than 1 for each.
- A .doc file's language is read, so spelling, hyphenation and LaTeX
  follow it; .doc files came back in no language at all.
- A .doc table row of more than 22 shaded cells keeps every cell's
  shading.
- A plain-text file in UTF-16 keeps what follows a NUL character, and a
  text file of many line breaks saves at once.
- On Windows, a window dragged by its title bar to the top of a screen
  is maximized on that screen again, whichever screen it is.

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
