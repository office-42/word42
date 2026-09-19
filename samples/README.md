# Sample documents

Documents for checking that Word42 reads what other word processors write.

## Where they come from

`feature-tour.odt` and `feature-tour.rtf` in this directory were written by
Word42 itself, from `samples/README.md`'s companion generator in the
project's scratchpad. Their content — the wording, the table, the drawn
picture — is the project's own, under the same GPL as the rest of Word42.

The files in `libreoffice/` and `abiword/` are that same document, opened
and saved again by:

| Directory | Written by | Formats |
| --- | --- | --- |
| `libreoffice/` | LibreOffice 7.5.3.2 | `.odt`, `.docx`, `.doc`, `.rtf`, `.html` (+ its picture) |
| `abiword/` | AbiWord 2 | `.abw`, `.rtf` |

Nothing here is copied from another project's test corpus. The documents
are conversions of the project's own content, so they carry no third
party's copyright, and they exercise the dialects those programs actually
write — which is the point: a file format is what the programs that write
it do, not only what the specification says.

To make them again, convert `feature-tour.odt`:

    soffice --headless --convert-to docx --outdir libreoffice feature-tour.odt
    AbiWord --to=abw --to-name=abiword/feature-tour.abw feature-tour.odt

## What the tour uses

Title and heading styles; bold, italic, underline, strikeout, colour,
highlight, small capitals and superscript; justified text with indents,
space above and one-and-a-half line spacing; a bulleted list and a
numbered one; tab stops with a dotted leader; a bordered and shaded
paragraph; a right-to-left Arabic paragraph; eight scripts in one line; a
three-by-three table with a header row; a picture; a footnote and an
endnote; a hyperlink and a bookmark; a header and a footer with page
fields; and the summary information.

## What comes through today

Checked by reading each file back and looking for every one of those
features. `-` marks what is missing, and why.

| File | What is missing | Whose it is |
| --- | --- | --- |
| `feature-tour.odt` (ours) | nothing | |
| `feature-tour.rtf` (ours) | nothing | |
| `libreoffice/feature-tour.odt` | nothing | |
| `libreoffice/feature-tour.docx` | nothing | |
| `libreoffice/feature-tour.rtf` | the Title style's **name** | LibreOffice writes style names in the user's language ("Tittel"); the formatting is read, and headings are recognised by their outline level |
| `libreoffice/feature-tour.doc` | bookmark, shading | the bookmark tables are Word42's `.doc` reader's remaining gap; the paragraph shading is not in the file, since LibreOffice's own `.doc` writer dropped it |
| `libreoffice/feature-tour.html` | the Title style's name, tabs, leaders, header, footer | none of those are in the file: HTML has no page furniture and no tab stops, and LibreOffice writes style names in the user's language |
| `abiword/feature-tour.abw` | small capitals, tabs, leaders, border | not in the file: AbiWord dropped them when it read the OpenDocument |
| `abiword/feature-tour.rtf` | small capitals, link, justify, spacing, tabs, leaders, border, shading, right-to-left | likewise, and AbiWord's RTF keeps less than its own format does |

Everything else — every feature in the list above, in every one of these
files — is read.

## A long document: the Bible

`bible-kjv.docx` is the whole of the King James Bible -- sixty-six
books, 31 102 verses, four million characters -- as a Word 2007
document: Word 2007's own defaults (Calibri 11 on the theme's minor
font, Cambria headings in Word's blues, ten points after each
paragraph, 1.15 lines), a Title, a Heading 1 for each book, a Heading 2
for each chapter, every verse a justified paragraph opened by its
number in superscript, a header, a page-number field in the footer, and
beside the opening of Genesis a picture of the first day, drawn for the
purpose and so the project's own work like the rest of its artwork.

It is here to be opened: a document of seventeen hundred pages is
what a word processor has to take in its stride, and this one loads in
a fifth of a second and lays out in three or four.  The text is the Authorized
Version of 1611, which is in the public domain; it was taken from the
plain-text King James in the scrollmapper `bible_databases` repository
on GitHub and set into WordprocessingML by a generator in the project's
scratchpad, the same way `feature-tour.odt` was made.

## Macros

`macros/` holds three macros in Word42 Basic, the VBA dialect Tools ▸
Macro runs: `Letterhead.bas` puts a title, the date and a greeting at
the top of a document, `WordReport.bas` has two Subs -- one reports the
document's counts in a message box, the other puts every "teh" right --
and `Emphasis.bas` toggles bold red on the selection. Copy them into the
`word42/macros` folder of your data directory (Tools ▸ Macro ▸ Macros
says where) and they appear in the Macro box.
