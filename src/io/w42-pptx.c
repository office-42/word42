/* w42-pptx.c - see w42-pptx.h
 *
 * Copyright (C) 2026 Andreas Røsdal
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "w42-pptx.h"

#include "w42-zip.h"
#include "w42-style.h"

#include <string.h>

/* ---------------------------------------------------------------------- */
/* The outline as slides                                                   */
/* ---------------------------------------------------------------------- */

static void
slide_free (gpointer data)
{
  W42Slide *slide = data;

  g_free (slide->title);
  g_ptr_array_free (slide->lines, TRUE);
  g_free (slide);
}

void
w42_slides_free (GPtrArray *slides)
{
  if (slides != NULL)
    g_ptr_array_free (slides, TRUE);
}

/* A line as a slide should show it: the marks that stand for something
 * else in a document -- a picture's place, a note's number, a tab -- have
 * no meaning on a slide, so they go or become a space. */
static char *
slide_line (const char *text)
{
  GString *out = g_string_new (NULL);

  for (const char *p = text; p != NULL && *p != '\0'; p = g_utf8_next_char (p))
    {
      gunichar c = g_utf8_get_char (p);

      if (c == 0xFFFC)          /* an object: a picture or a note's mark */
        continue;
      if (c == '\t' || c == 0x2028 || c == 0x000B)
        {
          if (out->len > 0 && out->str[out->len - 1] != ' ')
            g_string_append_c (out, ' ');
          continue;
        }
      if (c == 0x00AD)          /* a soft hyphen is for a line break */
        continue;
      g_string_append_unichar (out, c);
    }
  return g_strstrip (g_string_free (out, FALSE));
}

static W42Slide *
slide_new (const char *title, int level)
{
  W42Slide *slide = g_new0 (W42Slide, 1);

  slide->title = slide_line (title != NULL ? title : "");
  slide->lines = g_ptr_array_new_with_free_func (g_free);
  slide->level = level;
  return slide;
}

/* A heading starts a slide; the paragraphs under it are its lines.  Text
 * before the first heading belongs to a first slide of its own, whose
 * title is the document's title style if it has one. */
GPtrArray *
w42_slides_from_document (W42PieceTable *pt)
{
  GPtrArray *slides = g_ptr_array_new_with_free_func (slide_free);
  GPtrArray *blocks;
  W42StyleSheet *styles;
  W42Slide *current = NULL;

  g_return_val_if_fail (pt != NULL, slides);

  blocks = w42_pt_snapshot_blocks (pt);
  styles = w42_pt_stylesheet (pt);

  for (guint b = 0; b < blocks->len; b++)
    {
      const W42Block *block = g_ptr_array_index (blocks, b);
      const W42ParaFmt *pa = &w42_ap_table_get (w42_pt_ap_table (pt), block->ap)->pa;
      const char *text = block->text->str;
      int outline;

      /* Notes and the insides of tables are not part of the outline. */
      if (block->note >= 0 || block->table >= 0)
        continue;

      outline = pa->style != NULL ? w42_stylesheet_outline (styles, pa->style) : 0;
      if (outline == 0 && pa->style != NULL && g_ascii_strcasecmp (pa->style, "Title") == 0)
        outline = 1;

      if (outline > 0 && *g_strstrip ((char *) text) != '\0')
        {
          current = slide_new (text, outline);
          g_ptr_array_add (slides, current);
          continue;
        }

      if (*text == '\0')
        continue;

      if (current == NULL)
        {
          current = slide_new ("", 1);
          g_ptr_array_add (slides, current);
        }
      {
        char *line = slide_line (text);

        if (*line != '\0')
          g_ptr_array_add (current->lines, line);
        else
          g_free (line);
      }
    }

  g_ptr_array_free (blocks, TRUE);
  return slides;
}

/* ---------------------------------------------------------------------- */
/* Writing                                                                 */
/* ---------------------------------------------------------------------- */

static void
xml_text (GString *out, const char *text)
{
  for (const char *p = text; p != NULL && *p != '\0'; p++)
    {
      switch (*p)
        {
        case '&':  g_string_append (out, "&amp;");  break;
        case '<':  g_string_append (out, "&lt;");   break;
        case '>':  g_string_append (out, "&gt;");   break;
        case '"':  g_string_append (out, "&quot;"); break;
        case '\'': g_string_append (out, "&apos;"); break;
        default:
          if ((guchar) *p >= 0x20 || *p == '\t')
            g_string_append_c (out, *p);
          break;
        }
    }
}

#define XML_HEAD "<?xml version=\"1.0\" encoding=\"UTF-8\" standalone=\"yes\"?>\r\n"

/* A slide's XML: a title shape and a body shape, both plain text.  The
 * geometry is the 4:3 stage every presentation program understands, in
 * English metric units (914400 to the inch). */
static char *
slide_xml (const W42Slide *slide)
{
  GString *s = g_string_new (XML_HEAD);

  g_string_append (s,
    "<p:sld xmlns:a=\"http://schemas.openxmlformats.org/drawingml/2006/main\""
    " xmlns:r=\"http://schemas.openxmlformats.org/officeDocument/2006/relationships\""
    " xmlns:p=\"http://schemas.openxmlformats.org/presentationml/2006/main\">"
    "<p:cSld><p:spTree>"
    "<p:nvGrpSpPr><p:cNvPr id=\"1\" name=\"\"/><p:cNvGrpSpPr/><p:nvPr/></p:nvGrpSpPr>"
    "<p:grpSpPr><a:xfrm><a:off x=\"0\" y=\"0\"/><a:ext cx=\"0\" cy=\"0\"/>"
    "<a:chOff x=\"0\" y=\"0\"/><a:chExt cx=\"0\" cy=\"0\"/></a:xfrm></p:grpSpPr>");

  /* The title. */
  g_string_append (s,
    "<p:sp><p:nvSpPr><p:cNvPr id=\"2\" name=\"Title\"/>"
    "<p:cNvSpPr><a:spLocks noGrp=\"1\"/></p:cNvSpPr>"
    "<p:nvPr><p:ph type=\"title\"/></p:nvPr></p:nvSpPr>"
    "<p:spPr><a:xfrm><a:off x=\"685800\" y=\"457200\"/>"
    "<a:ext cx=\"7772400\" cy=\"1143000\"/></a:xfrm></p:spPr>"
    "<p:txBody><a:bodyPr/><a:lstStyle/><a:p><a:r><a:rPr lang=\"en-US\" dirty=\"0\"/><a:t>");
  xml_text (s, slide->title);
  g_string_append (s, "</a:t></a:r></a:p></p:txBody></p:sp>");

  /* The body: one paragraph per line. */
  g_string_append (s,
    "<p:sp><p:nvSpPr><p:cNvPr id=\"3\" name=\"Content\"/>"
    "<p:cNvSpPr><a:spLocks noGrp=\"1\"/></p:cNvSpPr>"
    "<p:nvPr><p:ph type=\"body\" idx=\"1\"/></p:nvPr></p:nvSpPr>"
    "<p:spPr><a:xfrm><a:off x=\"685800\" y=\"1828800\"/>"
    "<a:ext cx=\"7772400\" cy=\"4114800\"/></a:xfrm></p:spPr>"
    "<p:txBody><a:bodyPr/><a:lstStyle/>");
  if (slide->lines->len == 0)
    g_string_append (s, "<a:p><a:endParaRPr lang=\"en-US\"/></a:p>");
  for (guint i = 0; i < slide->lines->len; i++)
    {
      g_string_append (s, "<a:p><a:r><a:rPr lang=\"en-US\" dirty=\"0\"/><a:t>");
      xml_text (s, g_ptr_array_index (slide->lines, i));
      g_string_append (s, "</a:t></a:r></a:p>");
    }
  g_string_append (s, "</p:txBody></p:sp>");

  g_string_append (s, "</p:spTree></p:cSld><p:clrMapOvr><a:masterClrMapping/></p:clrMapOvr></p:sld>");
  return g_string_free (s, FALSE);
}

static const char *THEME_XML = XML_HEAD
  "<a:theme xmlns:a=\"http://schemas.openxmlformats.org/drawingml/2006/main\" name=\"Word42\">"
  "<a:themeElements>"
  "<a:clrScheme name=\"Word42\">"
  "<a:dk1><a:sysClr val=\"windowText\" lastClr=\"000000\"/></a:dk1>"
  "<a:lt1><a:sysClr val=\"window\" lastClr=\"FFFFFF\"/></a:lt1>"
  "<a:dk2><a:srgbClr val=\"000080\"/></a:dk2>"
  "<a:lt2><a:srgbClr val=\"C0C0C0\"/></a:lt2>"
  "<a:accent1><a:srgbClr val=\"000080\"/></a:accent1>"
  "<a:accent2><a:srgbClr val=\"808080\"/></a:accent2>"
  "<a:accent3><a:srgbClr val=\"C00000\"/></a:accent3>"
  "<a:accent4><a:srgbClr val=\"008000\"/></a:accent4>"
  "<a:accent5><a:srgbClr val=\"800080\"/></a:accent5>"
  "<a:accent6><a:srgbClr val=\"008080\"/></a:accent6>"
  "<a:hlink><a:srgbClr val=\"0000FF\"/></a:hlink>"
  "<a:folHlink><a:srgbClr val=\"800080\"/></a:folHlink>"
  "</a:clrScheme>"
  "<a:fontScheme name=\"Word42\">"
  "<a:majorFont><a:latin typeface=\"Arial\"/><a:ea typeface=\"\"/><a:cs typeface=\"\"/></a:majorFont>"
  "<a:minorFont><a:latin typeface=\"Arial\"/><a:ea typeface=\"\"/><a:cs typeface=\"\"/></a:minorFont>"
  "</a:fontScheme>"
  "<a:fmtScheme name=\"Word42\">"
  "<a:fillStyleLst>"
  "<a:solidFill><a:schemeClr val=\"phClr\"/></a:solidFill>"
  "<a:solidFill><a:schemeClr val=\"phClr\"/></a:solidFill>"
  "<a:solidFill><a:schemeClr val=\"phClr\"/></a:solidFill>"
  "</a:fillStyleLst>"
  "<a:lnStyleLst>"
  "<a:ln w=\"9525\"><a:solidFill><a:schemeClr val=\"phClr\"/></a:solidFill></a:ln>"
  "<a:ln w=\"9525\"><a:solidFill><a:schemeClr val=\"phClr\"/></a:solidFill></a:ln>"
  "<a:ln w=\"9525\"><a:solidFill><a:schemeClr val=\"phClr\"/></a:solidFill></a:ln>"
  "</a:lnStyleLst>"
  "<a:effectStyleLst>"
  "<a:effectStyle><a:effectLst/></a:effectStyle>"
  "<a:effectStyle><a:effectLst/></a:effectStyle>"
  "<a:effectStyle><a:effectLst/></a:effectStyle>"
  "</a:effectStyleLst>"
  "<a:bgFillStyleLst>"
  "<a:solidFill><a:schemeClr val=\"phClr\"/></a:solidFill>"
  "<a:solidFill><a:schemeClr val=\"phClr\"/></a:solidFill>"
  "<a:solidFill><a:schemeClr val=\"phClr\"/></a:solidFill>"
  "</a:bgFillStyleLst>"
  "</a:fmtScheme>"
  "</a:themeElements></a:theme>";

/* The master and the layout are as plain as the format allows: a title at
 * the top, a body under it, white behind both. */
static const char *MASTER_XML = XML_HEAD
  "<p:sldMaster xmlns:a=\"http://schemas.openxmlformats.org/drawingml/2006/main\""
  " xmlns:r=\"http://schemas.openxmlformats.org/officeDocument/2006/relationships\""
  " xmlns:p=\"http://schemas.openxmlformats.org/presentationml/2006/main\">"
  "<p:cSld><p:bg><p:bgPr><a:solidFill><a:schemeClr val=\"lt1\"/></a:solidFill>"
  "<a:effectLst/></p:bgPr></p:bg><p:spTree>"
  "<p:nvGrpSpPr><p:cNvPr id=\"1\" name=\"\"/><p:cNvGrpSpPr/><p:nvPr/></p:nvGrpSpPr>"
  "<p:grpSpPr><a:xfrm><a:off x=\"0\" y=\"0\"/><a:ext cx=\"0\" cy=\"0\"/>"
  "<a:chOff x=\"0\" y=\"0\"/><a:chExt cx=\"0\" cy=\"0\"/></a:xfrm></p:grpSpPr>"
  "<p:sp><p:nvSpPr><p:cNvPr id=\"2\" name=\"Title Placeholder\"/>"
  "<p:cNvSpPr><a:spLocks noGrp=\"1\"/></p:cNvSpPr><p:nvPr><p:ph type=\"title\"/></p:nvPr></p:nvSpPr>"
  "<p:spPr><a:xfrm><a:off x=\"685800\" y=\"457200\"/><a:ext cx=\"7772400\" cy=\"1143000\"/></a:xfrm>"
  "<a:prstGeom prst=\"rect\"><a:avLst/></a:prstGeom></p:spPr>"
  "<p:txBody><a:bodyPr/><a:lstStyle/><a:p><a:endParaRPr lang=\"en-US\"/></a:p></p:txBody></p:sp>"
  "<p:sp><p:nvSpPr><p:cNvPr id=\"3\" name=\"Text Placeholder\"/>"
  "<p:cNvSpPr><a:spLocks noGrp=\"1\"/></p:cNvSpPr>"
  "<p:nvPr><p:ph type=\"body\" idx=\"1\"/></p:nvPr></p:nvSpPr>"
  "<p:spPr><a:xfrm><a:off x=\"685800\" y=\"1828800\"/><a:ext cx=\"7772400\" cy=\"4114800\"/></a:xfrm>"
  "<a:prstGeom prst=\"rect\"><a:avLst/></a:prstGeom></p:spPr>"
  "<p:txBody><a:bodyPr/><a:lstStyle/><a:p><a:endParaRPr lang=\"en-US\"/></a:p></p:txBody></p:sp>"
  "</p:spTree></p:cSld>"
  "<p:clrMap bg1=\"lt1\" tx1=\"dk1\" bg2=\"lt2\" tx2=\"dk2\" accent1=\"accent1\" accent2=\"accent2\""
  " accent3=\"accent3\" accent4=\"accent4\" accent5=\"accent5\" accent6=\"accent6\""
  " hlink=\"hlink\" folHlink=\"folHlink\"/>"
  "<p:sldLayoutIdLst><p:sldLayoutId id=\"2147483649\" r:id=\"rId1\"/></p:sldLayoutIdLst>"
  "<p:txStyles>"
  "<p:titleStyle><a:lvl1pPr><a:defRPr sz=\"4000\" b=\"1\"/></a:lvl1pPr></p:titleStyle>"
  "<p:bodyStyle><a:lvl1pPr marL=\"342900\" indent=\"-342900\"><a:buChar char=\"\342\200\242\"/>"
  "<a:defRPr sz=\"2400\"/></a:lvl1pPr></p:bodyStyle>"
  "<p:otherStyle><a:lvl1pPr><a:defRPr sz=\"1800\"/></a:lvl1pPr></p:otherStyle>"
  "</p:txStyles></p:sldMaster>";

static const char *LAYOUT_XML = XML_HEAD
  "<p:sldLayout xmlns:a=\"http://schemas.openxmlformats.org/drawingml/2006/main\""
  " xmlns:r=\"http://schemas.openxmlformats.org/officeDocument/2006/relationships\""
  " xmlns:p=\"http://schemas.openxmlformats.org/presentationml/2006/main\""
  " type=\"obj\" preserve=\"1\">"
  "<p:cSld name=\"Title and Content\"><p:spTree>"
  "<p:nvGrpSpPr><p:cNvPr id=\"1\" name=\"\"/><p:cNvGrpSpPr/><p:nvPr/></p:nvGrpSpPr>"
  "<p:grpSpPr><a:xfrm><a:off x=\"0\" y=\"0\"/><a:ext cx=\"0\" cy=\"0\"/>"
  "<a:chOff x=\"0\" y=\"0\"/><a:chExt cx=\"0\" cy=\"0\"/></a:xfrm></p:grpSpPr>"
  "<p:sp><p:nvSpPr><p:cNvPr id=\"2\" name=\"Title\"/>"
  "<p:cNvSpPr><a:spLocks noGrp=\"1\"/></p:cNvSpPr><p:nvPr><p:ph type=\"title\"/></p:nvPr></p:nvSpPr>"
  "<p:spPr/><p:txBody><a:bodyPr/><a:lstStyle/><a:p><a:endParaRPr lang=\"en-US\"/></a:p></p:txBody></p:sp>"
  "<p:sp><p:nvSpPr><p:cNvPr id=\"3\" name=\"Content\"/>"
  "<p:cNvSpPr><a:spLocks noGrp=\"1\"/></p:cNvSpPr>"
  "<p:nvPr><p:ph type=\"body\" idx=\"1\"/></p:nvPr></p:nvSpPr>"
  "<p:spPr/><p:txBody><a:bodyPr/><a:lstStyle/><a:p><a:endParaRPr lang=\"en-US\"/></a:p></p:txBody></p:sp>"
  "</p:spTree></p:cSld><p:clrMapOvr><a:masterClrMapping/></p:clrMapOvr></p:sldLayout>";

gboolean
w42_pptx_save (W42PieceTable      *pt,
               const W42PageSetup *page,
               GFile              *file,
               GError            **error)
{
  W42ZipWriter *zip;
  GPtrArray *slides;
  GString *s;
  gboolean ok;

  g_return_val_if_fail (pt != NULL, FALSE);
  g_return_val_if_fail (G_IS_FILE (file), FALSE);

  (void) page;

  slides = w42_slides_from_document (pt);
  if (slides->len == 0)
    g_ptr_array_add (slides, slide_new ("", 1));

  zip = w42_zip_writer_new ();

  /* [Content_Types].xml */
  s = g_string_new (XML_HEAD);
  g_string_append (s,
    "<Types xmlns=\"http://schemas.openxmlformats.org/package/2006/content-types\">"
    "<Default Extension=\"rels\" ContentType=\"application/vnd.openxmlformats-package.relationships+xml\"/>"
    "<Default Extension=\"xml\" ContentType=\"application/xml\"/>"
    "<Override PartName=\"/ppt/presentation.xml\" ContentType=\"application/vnd.openxmlformats-officedocument.presentationml.presentation.main+xml\"/>"
    "<Override PartName=\"/ppt/slideMasters/slideMaster1.xml\" ContentType=\"application/vnd.openxmlformats-officedocument.presentationml.slideMaster+xml\"/>"
    "<Override PartName=\"/ppt/slideLayouts/slideLayout1.xml\" ContentType=\"application/vnd.openxmlformats-officedocument.presentationml.slideLayout+xml\"/>"
    "<Override PartName=\"/ppt/theme/theme1.xml\" ContentType=\"application/vnd.openxmlformats-officedocument.theme+xml\"/>");
  for (guint i = 0; i < slides->len; i++)
    g_string_append_printf (s,
      "<Override PartName=\"/ppt/slides/slide%u.xml\" ContentType=\"application/vnd.openxmlformats-officedocument.presentationml.slide+xml\"/>",
      i + 1);
  g_string_append (s, "</Types>");
  w42_zip_writer_add (zip, "[Content_Types].xml", s->str, s->len);
  g_string_free (s, TRUE);

  /* _rels/.rels */
  w42_zip_writer_add (zip, "_rels/.rels", XML_HEAD
    "<Relationships xmlns=\"http://schemas.openxmlformats.org/package/2006/relationships\">"
    "<Relationship Id=\"rId1\" Type=\"http://schemas.openxmlformats.org/officeDocument/2006/relationships/officeDocument\""
    " Target=\"ppt/presentation.xml\"/></Relationships>",
    strlen (XML_HEAD) + 245);

  /* ppt/presentation.xml */
  s = g_string_new (XML_HEAD);
  g_string_append (s,
    "<p:presentation xmlns:a=\"http://schemas.openxmlformats.org/drawingml/2006/main\""
    " xmlns:r=\"http://schemas.openxmlformats.org/officeDocument/2006/relationships\""
    " xmlns:p=\"http://schemas.openxmlformats.org/presentationml/2006/main\">"
    "<p:sldMasterIdLst><p:sldMasterId id=\"2147483648\" r:id=\"rId1\"/></p:sldMasterIdLst>"
    "<p:sldIdLst>");
  for (guint i = 0; i < slides->len; i++)
    g_string_append_printf (s, "<p:sldId id=\"%u\" r:id=\"rId%u\"/>", 256 + i, i + 2);
  g_string_append (s,
    "</p:sldIdLst>"
    "<p:sldSz cx=\"9144000\" cy=\"6858000\" type=\"screen4x3\"/>"
    "<p:notesSz cx=\"6858000\" cy=\"9144000\"/></p:presentation>");
  w42_zip_writer_add (zip, "ppt/presentation.xml", s->str, s->len);
  g_string_free (s, TRUE);

  /* ppt/_rels/presentation.xml.rels */
  s = g_string_new (XML_HEAD);
  g_string_append (s,
    "<Relationships xmlns=\"http://schemas.openxmlformats.org/package/2006/relationships\">"
    "<Relationship Id=\"rId1\" Type=\"http://schemas.openxmlformats.org/officeDocument/2006/relationships/slideMaster\""
    " Target=\"slideMasters/slideMaster1.xml\"/>");
  for (guint i = 0; i < slides->len; i++)
    g_string_append_printf (s,
      "<Relationship Id=\"rId%u\" Type=\"http://schemas.openxmlformats.org/officeDocument/2006/relationships/slide\""
      " Target=\"slides/slide%u.xml\"/>", i + 2, i + 1);
  g_string_append_printf (s,
    "<Relationship Id=\"rId%u\" Type=\"http://schemas.openxmlformats.org/officeDocument/2006/relationships/theme\""
    " Target=\"theme/theme1.xml\"/></Relationships>", slides->len + 2);
  w42_zip_writer_add (zip, "ppt/_rels/presentation.xml.rels", s->str, s->len);
  g_string_free (s, TRUE);

  /* The master, its layout and the theme. */
  w42_zip_writer_add (zip, "ppt/slideMasters/slideMaster1.xml", MASTER_XML, strlen (MASTER_XML));
  w42_zip_writer_add (zip, "ppt/slideMasters/_rels/slideMaster1.xml.rels", XML_HEAD
    "<Relationships xmlns=\"http://schemas.openxmlformats.org/package/2006/relationships\">"
    "<Relationship Id=\"rId1\" Type=\"http://schemas.openxmlformats.org/officeDocument/2006/relationships/slideLayout\""
    " Target=\"../slideLayouts/slideLayout1.xml\"/>"
    "<Relationship Id=\"rId2\" Type=\"http://schemas.openxmlformats.org/officeDocument/2006/relationships/theme\""
    " Target=\"../theme/theme1.xml\"/></Relationships>",
    strlen (XML_HEAD) + 424);
  w42_zip_writer_add (zip, "ppt/slideLayouts/slideLayout1.xml", LAYOUT_XML, strlen (LAYOUT_XML));
  w42_zip_writer_add (zip, "ppt/slideLayouts/_rels/slideLayout1.xml.rels", XML_HEAD
    "<Relationships xmlns=\"http://schemas.openxmlformats.org/package/2006/relationships\">"
    "<Relationship Id=\"rId1\" Type=\"http://schemas.openxmlformats.org/officeDocument/2006/relationships/slideMaster\""
    " Target=\"../slideMasters/slideMaster1.xml\"/></Relationships>",
    strlen (XML_HEAD) + 253);
  w42_zip_writer_add (zip, "ppt/theme/theme1.xml", THEME_XML, strlen (THEME_XML));

  /* The slides. */
  for (guint i = 0; i < slides->len; i++)
    {
      char *name = g_strdup_printf ("ppt/slides/slide%u.xml", i + 1);
      char *rels = g_strdup_printf ("ppt/slides/_rels/slide%u.xml.rels", i + 1);
      char *xml = slide_xml (g_ptr_array_index (slides, i));
      static const char *SLIDE_RELS = XML_HEAD
        "<Relationships xmlns=\"http://schemas.openxmlformats.org/package/2006/relationships\">"
        "<Relationship Id=\"rId1\" Type=\"http://schemas.openxmlformats.org/officeDocument/2006/relationships/slideLayout\""
        " Target=\"../slideLayouts/slideLayout1.xml\"/></Relationships>";

      w42_zip_writer_add (zip, name, xml, strlen (xml));
      w42_zip_writer_add (zip, rels, SLIDE_RELS, strlen (SLIDE_RELS));
      g_free (xml);
      g_free (rels);
      g_free (name);
    }

  ok = w42_zip_writer_save (zip, file, error);
  w42_zip_writer_free (zip);
  w42_slides_free (slides);
  return ok;
}

/* ---------------------------------------------------------------------- */
/* Reading                                                                 */
/* ---------------------------------------------------------------------- */

typedef struct {
  GString  *text;          /* the paragraph being read */
  GPtrArray *paras;        /* char *: the shape's paragraphs */
  gboolean  in_text;       /* inside <a:t> */
  gboolean  is_title;      /* the shape holds the slide's title */
  gboolean  in_shape;
  GPtrArray *titles;       /* char * per shape, in order */
  GPtrArray *bodies;       /* GPtrArray of char * per shape */
} SlideReader;

static void
slide_start (GMarkupParseContext *ctx, const char *name, const char **an,
             const char **av, gpointer data, GError **error)
{
  SlideReader *r = data;

  (void) ctx; (void) error;

  if (g_str_has_suffix (name, ":sp") || g_str_equal (name, "sp"))
    {
      r->in_shape = TRUE;
      r->is_title = FALSE;
      r->paras = g_ptr_array_new_with_free_func (g_free);
    }
  else if (g_str_has_suffix (name, ":ph"))
    {
      const char *type = NULL;

      for (guint i = 0; an != NULL && an[i] != NULL; i++)
        if (g_str_equal (an[i], "type"))
          type = av[i];
      if (type != NULL && (g_str_equal (type, "title") || g_str_equal (type, "ctrTitle")))
        r->is_title = TRUE;
    }
  else if (g_str_has_suffix (name, ":t"))
    r->in_text = TRUE;
  else if (g_str_has_suffix (name, ":br"))
    g_string_append_c (r->text, ' ');
}

static void
slide_end (GMarkupParseContext *ctx, const char *name, gpointer data, GError **error)
{
  SlideReader *r = data;

  (void) ctx; (void) error;

  if (g_str_has_suffix (name, ":t"))
    r->in_text = FALSE;
  else if (g_str_has_suffix (name, ":p") && !g_str_has_suffix (name, ":sp"))
    {
      if (r->paras != NULL)
        g_ptr_array_add (r->paras, g_strdup (g_strstrip (r->text->str)));
      g_string_truncate (r->text, 0);
    }
  else if (g_str_has_suffix (name, ":sp") || g_str_equal (name, "sp"))
    {
      if (r->paras != NULL)
        {
          if (r->is_title)
            {
              GString *joined = g_string_new (NULL);

              for (guint i = 0; i < r->paras->len; i++)
                {
                  const char *line = g_ptr_array_index (r->paras, i);

                  if (*line == '\0')
                    continue;
                  if (joined->len > 0)
                    g_string_append_c (joined, ' ');
                  g_string_append (joined, line);
                }
              g_ptr_array_add (r->titles, g_string_free (joined, FALSE));
              g_ptr_array_free (r->paras, TRUE);
            }
          else
            g_ptr_array_add (r->bodies, r->paras);
          r->paras = NULL;
        }
      r->in_shape = FALSE;
    }
}

static void
slide_text (GMarkupParseContext *ctx, const char *text, gsize len,
            gpointer data, GError **error)
{
  SlideReader *r = data;

  (void) ctx; (void) error;
  if (r->in_text)
    g_string_append_len (r->text, text, len);
}

/* The slide names in the order the presentation lists them. */
static GPtrArray *
slide_order (W42Zip *zip)
{
  GPtrArray *names = g_ptr_array_new_with_free_func (g_free);
  GBytes *rels = w42_zip_read (zip, "ppt/_rels/presentation.xml.rels");
  GBytes *pres = w42_zip_read (zip, "ppt/presentation.xml");
  GHashTable *by_id = g_hash_table_new_full (g_str_hash, g_str_equal, g_free, g_free);

  if (rels != NULL)
    {
      gsize len = 0;
      const char *d = g_bytes_get_data (rels, &len);
      const char *p = d;

      while (p != NULL && (p = g_strstr_len (p, d + len - p, "<Relationship ")) != NULL)
        {
          const char *gt = strchr (p, '>');
          char *id = NULL, *target = NULL;
          const char *q;

          if (gt == NULL)
            break;
          if ((q = g_strstr_len (p, gt - p, "Id=\"")) != NULL)
            {
              const char *e = strchr (q + 4, '"');
              if (e != NULL) id = g_strndup (q + 4, (gsize) (e - q - 4));
            }
          if ((q = g_strstr_len (p, gt - p, "Target=\"")) != NULL)
            {
              const char *e = strchr (q + 8, '"');
              if (e != NULL) target = g_strndup (q + 8, (gsize) (e - q - 8));
            }
          if (id != NULL && target != NULL && strstr (target, "slides/") != NULL)
            g_hash_table_insert (by_id, id, g_strconcat ("ppt/", target, NULL));
          else
            g_free (id);
          g_free (target);
          p = gt + 1;
        }
      g_bytes_unref (rels);
    }

  if (pres != NULL)
    {
      gsize len = 0;
      const char *d = g_bytes_get_data (pres, &len);
      const char *p = d;

      while ((p = g_strstr_len (p, d + len - p, "r:id=\"")) != NULL)
        {
          const char *e = strchr (p + 6, '"');
          char *id;
          const char *target;

          if (e == NULL)
            break;
          id = g_strndup (p + 6, (gsize) (e - p - 6));
          target = g_hash_table_lookup (by_id, id);
          if (target != NULL)
            g_ptr_array_add (names, g_strdup (target));
          g_free (id);
          p = e + 1;
        }
      g_bytes_unref (pres);
    }

  /* No presentation part worth the name: take the slides as they come. */
  if (names->len == 0)
    for (guint i = 1; i < 500; i++)
      {
        char *name = g_strdup_printf ("ppt/slides/slide%u.xml", i);

        if (!w42_zip_has (zip, name))
          {
            g_free (name);
            break;
          }
        g_ptr_array_add (names, name);
      }

  g_hash_table_destroy (by_id);
  return names;
}

gboolean
w42_pptx_load (W42PieceTable *pt,
               W42PageSetup  *page,
               GFile         *file,
               GError       **error)
{
  W42Zip *zip;
  GPtrArray *names;
  GString *text;

  g_return_val_if_fail (pt != NULL, FALSE);
  g_return_val_if_fail (G_IS_FILE (file), FALSE);

  zip = w42_zip_open (file, error);
  if (zip == NULL)
    return FALSE;

  if (!w42_zip_has (zip, "ppt/presentation.xml"))
    {
      w42_zip_free (zip);
      g_set_error (error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA,
                   "That file is not a presentation: it has no ppt/presentation.xml.");
      return FALSE;
    }

  if (page != NULL && page->width == 0)
    {
      page->width = 12240; page->height = 15840;
      page->margin_left = page->margin_right = 1440;
      page->margin_top = page->margin_bottom = 1440;
    }

  names = slide_order (zip);
  text = g_string_new (NULL);

  /* The deck as an outline: each slide's title a heading, its lines the
   * paragraphs under it.  The headings are styled after the text is in. */
  {
    GArray *heading_at = g_array_new (FALSE, FALSE, sizeof (guint));

    for (guint i = 0; i < names->len; i++)
      {
        GBytes *bytes = w42_zip_read (zip, g_ptr_array_index (names, i));
        SlideReader r;
        GMarkupParser parser = { slide_start, slide_end, slide_text, NULL, NULL };
        GMarkupParseContext *ctx;
        gsize len = 0;
        const char *d;

        if (bytes == NULL)
          continue;
        d = g_bytes_get_data (bytes, &len);

        memset (&r, 0, sizeof r);
        r.text = g_string_new (NULL);
        r.titles = g_ptr_array_new_with_free_func (g_free);
        r.bodies = g_ptr_array_new_with_free_func ((GDestroyNotify) g_ptr_array_unref);

        ctx = g_markup_parse_context_new (&parser, 0, &r, NULL);
        if (g_markup_parse_context_parse (ctx, d, len, NULL))
          g_markup_parse_context_end_parse (ctx, NULL);
        g_markup_parse_context_free (ctx);

        {
          guint line_no = (guint) 0;
          const char *title = r.titles->len > 0 ? g_ptr_array_index (r.titles, 0) : NULL;

          (void) line_no;
          if (text->len > 0)
            g_string_append_c (text, '\n');
          {
            guint at = 0;

            for (const char *p = text->str; *p != '\0'; p++)
              if (*p == '\n')
                at++;
            g_array_append_val (heading_at, at);
          }
          g_string_append (text, title != NULL && *title != '\0' ? title : "Slide");

          for (guint b = 0; b < r.bodies->len; b++)
            {
              GPtrArray *paras = g_ptr_array_index (r.bodies, b);

              for (guint k = 0; k < paras->len; k++)
                {
                  const char *line = g_ptr_array_index (paras, k);

                  if (*line == '\0')
                    continue;
                  g_string_append_c (text, '\n');
                  g_string_append (text, line);
                }
            }
        }

        g_string_free (r.text, TRUE);
        g_ptr_array_free (r.titles, TRUE);
        g_ptr_array_free (r.bodies, TRUE);
        if (r.paras != NULL)
          g_ptr_array_free (r.paras, TRUE);
        g_bytes_unref (bytes);
      }

    w42_pt_load_text (pt, text->str);

    /* Each slide's title is a heading, which is what makes the document
     * an outline again -- and what lets it be shown as slides. */
    {
      GPtrArray *blocks = w42_pt_snapshot_blocks (pt);

      for (guint i = 0; i < heading_at->len; i++)
        {
          guint b = g_array_index (heading_at, guint, i);

          if (b < blocks->len)
            {
              const W42Block *block = g_ptr_array_index (blocks, b);

              w42_pt_apply_style (pt, block->start_pos + 1, 1, "Heading 1");
            }
        }
      g_ptr_array_free (blocks, TRUE);
    }
    g_array_free (heading_at, TRUE);
  }

  g_string_free (text, TRUE);
  g_ptr_array_free (names, TRUE);
  w42_zip_free (zip);
  return TRUE;
}
