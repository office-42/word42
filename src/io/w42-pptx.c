/* w42-pptx.c - see w42-pptx.h
 *
 * Copyright (C) 2026 Andreas Røsdal
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "w42-pptx.h"

#include "w42-build.h"
#include "w42-image.h"
#include "w42-zip.h"
#include "w42-style.h"

#include <stdlib.h>
#include <string.h>
#include <glib/gi18n.h>

/* ---------------------------------------------------------------------- */
/* The outline as slides                                                   */
/* ---------------------------------------------------------------------- */

static void
picture_free (gpointer data)
{
  W42SlidePicture *pic = data;

  g_bytes_unref (pic->data);
  g_free (pic);
}

static void
slide_free (gpointer data)
{
  W42Slide *slide = data;

  g_free (slide->title);
  g_ptr_array_free (slide->lines, TRUE);
  g_array_free (slide->levels, TRUE);
  g_ptr_array_free (slide->pictures, TRUE);
  g_free (slide->notes);
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
  slide->levels = g_array_new (FALSE, FALSE, sizeof (int));
  slide->pictures = g_ptr_array_new_with_free_func (picture_free);
  slide->level = level;
  return slide;
}

/* How deep a line sits: its list level, or, for a paragraph out of a
 * list, a level for every half inch it is indented. */
static int
line_level (const W42ParaFmt *pa)
{
  if (pa->list != W42_LIST_NONE)
    return MIN (pa->list_level, 4);
  return CLAMP (pa->indent_left / 720, 0, 4);
}

/* The pictures and comments of a paragraph go to its slide: the
 * pictures to be shown, the comments as the speaker's notes. */
static void
slide_gather (W42PieceTable *pt, W42Slide *slide, const W42Block *block, GString *notes)
{
  W42ObjectTable *objects = w42_pt_object_table (pt);
  const char *last_comment = NULL;

  for (guint i = 0; i < block->runs->len; i++)
    {
      const W42Run *run = &g_array_index (block->runs, W42Run, i);
      const char *comment = w42_ap_table_get (w42_pt_ap_table (pt), run->ap)->ch.comment;

      if (comment != NULL && *comment != '\0' && comment != last_comment &&
          strstr (notes->str, comment) == NULL)
        {
          if (notes->len > 0)
            g_string_append_c (notes, '\n');
          g_string_append (notes, comment);
        }
      last_comment = comment;

      if (run->object != W42_OBJECT_NONE && run->footnote == 0)
        {
          const W42Object *obj = w42_object_table_get (objects, run->object);

          if (obj != NULL && obj->data != NULL && obj->pixel_w > 0 && obj->pixel_h > 0)
            {
              W42SlidePicture *pic = g_new0 (W42SlidePicture, 1);

              pic->data = g_bytes_ref (obj->data);
              pic->format = obj->format;
              pic->pixel_w = obj->pixel_w;
              pic->pixel_h = obj->pixel_h;
              g_ptr_array_add (slide->pictures, pic);
            }
        }
    }
}

static void
slide_close (W42Slide *slide, GString *notes)
{
  if (slide != NULL && notes->len > 0)
    slide->notes = g_strdup (notes->str);
  g_string_truncate (notes, 0);
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
  GString *notes;

  g_return_val_if_fail (pt != NULL, slides);

  blocks = w42_pt_snapshot_blocks (pt);
  styles = w42_pt_stylesheet (pt);
  notes = g_string_new (NULL);

  for (guint b = 0; b < blocks->len; b++)
    {
      const W42Block *block = g_ptr_array_index (blocks, b);
      const W42ParaFmt *pa = &w42_ap_table_get (w42_pt_ap_table (pt), block->ap)->pa;
      const char *text = block->text->str;
      gboolean is_title;
      int outline;

      /* Notes and the insides of tables are not part of the outline. */
      if (block->note >= 0 || block->table >= 0)
        continue;

      is_title = pa->style != NULL && g_ascii_strcasecmp (pa->style, "Title") == 0;
      outline = pa->style != NULL ? w42_stylesheet_outline (styles, pa->style) : 0;
      if (outline == 0 && is_title)
        outline = 1;

      if (outline > 0)
        {
          char *title = slide_line (text);

          if (*title != '\0')
            {
              slide_close (current, notes);
              current = slide_new (title, outline);
              current->title_slide = is_title;
              g_ptr_array_add (slides, current);
              slide_gather (pt, current, block, notes);
              g_free (title);
              continue;
            }
          g_free (title);
        }

      if (*text == '\0')
        continue;

      if (current == NULL)
        {
          current = slide_new ("", 1);
          g_ptr_array_add (slides, current);
        }
      slide_gather (pt, current, block, notes);
      {
        char *line = slide_line (text);

        if (*line != '\0')
          {
            int level = line_level (pa);

            g_ptr_array_add (current->lines, line);
            g_array_append_val (current->levels, level);
          }
        else
          g_free (line);
      }
    }
  slide_close (current, notes);

  g_string_free (notes, TRUE);
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
#define NS_ALL " xmlns:a=\"http://schemas.openxmlformats.org/drawingml/2006/main\"" \
               " xmlns:r=\"http://schemas.openxmlformats.org/officeDocument/2006/relationships\"" \
               " xmlns:p=\"http://schemas.openxmlformats.org/presentationml/2006/main\""
#define REL_NS "http://schemas.openxmlformats.org/officeDocument/2006/relationships/"
#define GROUP_PROPS \
  "<p:nvGrpSpPr><p:cNvPr id=\"1\" name=\"\"/><p:cNvGrpSpPr/><p:nvPr/></p:nvGrpSpPr>" \
  "<p:grpSpPr><a:xfrm><a:off x=\"0\" y=\"0\"/><a:ext cx=\"0\" cy=\"0\"/>" \
  "<a:chOff x=\"0\" y=\"0\"/><a:chExt cx=\"0\" cy=\"0\"/></a:xfrm></p:grpSpPr>"
#define CLR_MAP \
  "<p:clrMap bg1=\"lt1\" tx1=\"dk1\" bg2=\"lt2\" tx2=\"dk2\" accent1=\"accent1\" accent2=\"accent2\"" \
  " accent3=\"accent3\" accent4=\"accent4\" accent5=\"accent5\" accent6=\"accent6\"" \
  " hlink=\"hlink\" folHlink=\"folHlink\"/>"

/* The stage is 16:9, PowerPoint's own default since 2013, in English
 * metric units (914400 to the inch).  The boxes are where PowerPoint puts
 * its title and its content. */
#define SLIDE_W 12192000
#define SLIDE_H 6858000
#define BODY_X  838200
#define BODY_Y  1825625
#define BODY_W  10515600
#define BODY_H  4351338
#define HALF_W  5181600
#define PICS_X  (BODY_X + BODY_W - HALF_W)

typedef struct {
  guint  number;         /* 1-based */
  GPtrArray *media;      /* char *: the package names of its pictures */
} SlideParts;

/* One paragraph of a text box, at a level. */
static void
para_xml (GString *s, const char *text, int level)
{
  g_string_append (s, "<a:p>");
  if (level > 0)
    g_string_append_printf (s, "<a:pPr lvl=\"%d\"/>", MIN (level, 4));
  g_string_append (s, "<a:r><a:rPr lang=\"en-US\" dirty=\"0\"/><a:t>");
  xml_text (s, text);
  g_string_append (s, "</a:t></a:r></a:p>");
}

static void
text_shape (GString *s, guint id, const char *name, const char *ph, const char *text_xml,
            gboolean placed, gint64 x, gint64 y, gint64 w, gint64 h)
{
  g_string_append_printf (s,
    "<p:sp><p:nvSpPr><p:cNvPr id=\"%u\" name=\"%s\"/>"
    "<p:cNvSpPr><a:spLocks noGrp=\"1\"/></p:cNvSpPr>"
    "<p:nvPr>%s</p:nvPr></p:nvSpPr>", id, name, ph);
  if (placed)
    g_string_append_printf (s,
      "<p:spPr><a:xfrm><a:off x=\"%" G_GINT64_FORMAT "\" y=\"%" G_GINT64_FORMAT "\"/>"
      "<a:ext cx=\"%" G_GINT64_FORMAT "\" cy=\"%" G_GINT64_FORMAT "\"/></a:xfrm></p:spPr>",
      x, y, w, h);
  else
    g_string_append (s, "<p:spPr/>");
  g_string_append (s, "<p:txBody><a:bodyPr><a:normAutofit/></a:bodyPr><a:lstStyle/>");
  g_string_append (s, text_xml != NULL && *text_xml != '\0'
                        ? text_xml : "<a:p><a:endParaRPr lang=\"en-US\" dirty=\"0\"/></a:p>");
  g_string_append (s, "</p:txBody></p:sp>");
}

/* The pictures side by side in the box, each as large as its share lets
 * it be without changing shape, centred in that share. */
static void
pictures_xml (GString *s, const W42Slide *slide, guint first_id,
              gint64 bx, gint64 by, gint64 bw, gint64 bh)
{
  guint n = slide->pictures->len;
  gint64 gap = n > 1 ? 152400 : 0;
  gint64 cell_w = (bw - gap * (n - 1)) / MAX (n, 1);

  for (guint i = 0; i < n; i++)
    {
      const W42SlidePicture *pic = g_ptr_array_index (slide->pictures, i);
      double scale = MIN ((double) cell_w / pic->pixel_w, (double) bh / pic->pixel_h);
      gint64 w = (gint64) (pic->pixel_w * scale), h = (gint64) (pic->pixel_h * scale);
      gint64 x = bx + i * (cell_w + gap) + (cell_w - w) / 2;
      gint64 y = by + (bh - h) / 2;

      g_string_append_printf (s,
        "<p:pic><p:nvPicPr><p:cNvPr id=\"%u\" name=\"Picture %u\"/>"
        "<p:cNvPicPr><a:picLocks noChangeAspect=\"1\"/></p:cNvPicPr><p:nvPr/></p:nvPicPr>"
        "<p:blipFill><a:blip r:embed=\"rId%u\"/><a:stretch><a:fillRect/></a:stretch></p:blipFill>"
        "<p:spPr><a:xfrm><a:off x=\"%" G_GINT64_FORMAT "\" y=\"%" G_GINT64_FORMAT "\"/>"
        "<a:ext cx=\"%" G_GINT64_FORMAT "\" cy=\"%" G_GINT64_FORMAT "\"/></a:xfrm>"
        "<a:prstGeom prst=\"rect\"><a:avLst/></a:prstGeom></p:spPr></p:pic>",
        first_id + i, i + 1, i + 2, x, y, w, h);
    }
}

/* Whether the slide goes out on the Title Slide layout: a title slide
 * with no pictures to find room for. */
static gboolean
uses_title_layout (const W42Slide *slide)
{
  return slide->title_slide && slide->pictures->len == 0;
}

static char *
slide_xml (const W42Slide *slide)
{
  GString *s = g_string_new (XML_HEAD);
  GString *body = g_string_new (NULL);
  GString *title = g_string_new (NULL);

  g_string_append (s, "<p:sld" NS_ALL "><p:cSld><p:spTree>" GROUP_PROPS);

  if (*slide->title != '\0')
    para_xml (title, slide->title, 0);

  if (uses_title_layout (slide))
    {
      for (guint i = 0; i < slide->lines->len; i++)
        para_xml (body, g_ptr_array_index (slide->lines, i), 0);
      text_shape (s, 2, "Title 1", "<p:ph type=\"ctrTitle\"/>", title->str, FALSE, 0, 0, 0, 0);
      text_shape (s, 3, "Subtitle 2", "<p:ph type=\"subTitle\" idx=\"1\"/>", body->str,
                  FALSE, 0, 0, 0, 0);
    }
  else
    {
      gboolean pictures = slide->pictures->len > 0;
      gboolean lines = slide->lines->len > 0;

      for (guint i = 0; i < slide->lines->len; i++)
        para_xml (body, g_ptr_array_index (slide->lines, i),
                  g_array_index (slide->levels, int, i));
      text_shape (s, 2, "Title 1", "<p:ph type=\"title\"/>", title->str, FALSE, 0, 0, 0, 0);
      /* With pictures, the text takes the left half and they the right;
       * without text, they have the whole of the content box. */
      if (lines || !pictures)
        text_shape (s, 3, "Content 2", "<p:ph idx=\"1\"/>", body->str,
                    pictures, BODY_X, BODY_Y, HALF_W, BODY_H);
      if (pictures)
        pictures_xml (s, slide, 4,
                      lines ? PICS_X : BODY_X, BODY_Y, lines ? HALF_W : BODY_W, BODY_H);
    }

  g_string_append (s, "</p:spTree></p:cSld><p:clrMapOvr><a:masterClrMapping/></p:clrMapOvr></p:sld>");
  g_string_free (body, TRUE);
  g_string_free (title, TRUE);
  return g_string_free (s, FALSE);
}

/* The speaker's notes, a paragraph a line, under a picture of the slide. */
static char *
notes_xml (const char *notes)
{
  GString *s = g_string_new (XML_HEAD);
  char **lines = g_strsplit (notes, "\n", -1);

  g_string_append (s, "<p:notes" NS_ALL "><p:cSld><p:spTree>" GROUP_PROPS
    "<p:sp><p:nvSpPr><p:cNvPr id=\"2\" name=\"Slide Image Placeholder 1\"/>"
    "<p:cNvSpPr><a:spLocks noGrp=\"1\" noRot=\"1\" noChangeAspect=\"1\"/></p:cNvSpPr>"
    "<p:nvPr><p:ph type=\"sldImg\"/></p:nvPr></p:nvSpPr><p:spPr/></p:sp>"
    "<p:sp><p:nvSpPr><p:cNvPr id=\"3\" name=\"Notes Placeholder 2\"/>"
    "<p:cNvSpPr><a:spLocks noGrp=\"1\"/></p:cNvSpPr>"
    "<p:nvPr><p:ph type=\"body\" idx=\"1\"/></p:nvPr></p:nvSpPr><p:spPr/>"
    "<p:txBody><a:bodyPr/><a:lstStyle/>");
  for (guint i = 0; lines[i] != NULL; i++)
    para_xml (s, lines[i], 0);
  g_string_append (s, "</p:txBody></p:sp></p:spTree></p:cSld>"
                      "<p:clrMapOvr><a:masterClrMapping/></p:clrMapOvr></p:notes>");
  g_strfreev (lines);
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

/* A body level: a bullet, indented a quarter inch further each level
 * down, in type a step smaller. */
#define BODY_LEVEL(n, marl, bullet, size) \
  "<a:lvl" #n "pPr marL=\"" #marl "\" indent=\"-228600\">" \
  "<a:spcBef><a:spcPts val=\"1000\"/></a:spcBef>" \
  "<a:buFont typeface=\"Arial\"/><a:buChar char=\"" bullet "\"/>" \
  "<a:defRPr sz=\"" #size "\"><a:solidFill><a:schemeClr val=\"tx1\"/></a:solidFill>" \
  "<a:latin typeface=\"+mn-lt\"/></a:defRPr></a:lvl" #n "pPr>"

/* The master: a title at the top, a body under it, white behind both. */
static const char *MASTER_XML = XML_HEAD
  "<p:sldMaster" NS_ALL ">"
  "<p:cSld><p:bg><p:bgPr><a:solidFill><a:schemeClr val=\"bg1\"/></a:solidFill>"
  "<a:effectLst/></p:bgPr></p:bg><p:spTree>" GROUP_PROPS
  "<p:sp><p:nvSpPr><p:cNvPr id=\"2\" name=\"Title Placeholder 1\"/>"
  "<p:cNvSpPr><a:spLocks noGrp=\"1\"/></p:cNvSpPr><p:nvPr><p:ph type=\"title\"/></p:nvPr></p:nvSpPr>"
  "<p:spPr><a:xfrm><a:off x=\"838200\" y=\"365125\"/><a:ext cx=\"10515600\" cy=\"1325563\"/></a:xfrm>"
  "<a:prstGeom prst=\"rect\"><a:avLst/></a:prstGeom></p:spPr>"
  "<p:txBody><a:bodyPr anchor=\"ctr\"><a:normAutofit/></a:bodyPr><a:lstStyle/>"
  "<a:p><a:endParaRPr lang=\"en-US\"/></a:p></p:txBody></p:sp>"
  "<p:sp><p:nvSpPr><p:cNvPr id=\"3\" name=\"Text Placeholder 2\"/>"
  "<p:cNvSpPr><a:spLocks noGrp=\"1\"/></p:cNvSpPr>"
  "<p:nvPr><p:ph type=\"body\" idx=\"1\"/></p:nvPr></p:nvSpPr>"
  "<p:spPr><a:xfrm><a:off x=\"838200\" y=\"1825625\"/><a:ext cx=\"10515600\" cy=\"4351338\"/></a:xfrm>"
  "<a:prstGeom prst=\"rect\"><a:avLst/></a:prstGeom></p:spPr>"
  "<p:txBody><a:bodyPr><a:normAutofit/></a:bodyPr><a:lstStyle/>"
  "<a:p><a:endParaRPr lang=\"en-US\"/></a:p></p:txBody></p:sp>"
  "</p:spTree></p:cSld>" CLR_MAP
  "<p:sldLayoutIdLst><p:sldLayoutId id=\"2147483649\" r:id=\"rId1\"/>"
  "<p:sldLayoutId id=\"2147483650\" r:id=\"rId2\"/></p:sldLayoutIdLst>"
  "<p:txStyles>"
  "<p:titleStyle><a:lvl1pPr algn=\"l\"><a:defRPr sz=\"4400\">"
  "<a:solidFill><a:schemeClr val=\"tx2\"/></a:solidFill><a:latin typeface=\"+mj-lt\"/>"
  "</a:defRPr></a:lvl1pPr></p:titleStyle>"
  "<p:bodyStyle>"
  BODY_LEVEL (1, 228600, "\342\200\242", 2800)
  BODY_LEVEL (2, 685800, "\342\200\223", 2400)
  BODY_LEVEL (3, 1143000, "\342\200\242", 2000)
  BODY_LEVEL (4, 1600200, "\342\200\223", 1800)
  BODY_LEVEL (5, 2057400, "\302\273", 1800)
  "</p:bodyStyle>"
  "<p:otherStyle><a:lvl1pPr><a:defRPr sz=\"1800\"/></a:lvl1pPr></p:otherStyle>"
  "</p:txStyles></p:sldMaster>";

static const char *LAYOUT_CONTENT_XML = XML_HEAD
  "<p:sldLayout" NS_ALL " type=\"obj\" preserve=\"1\">"
  "<p:cSld name=\"Title and Content\"><p:spTree>" GROUP_PROPS
  "<p:sp><p:nvSpPr><p:cNvPr id=\"2\" name=\"Title 1\"/>"
  "<p:cNvSpPr><a:spLocks noGrp=\"1\"/></p:cNvSpPr><p:nvPr><p:ph type=\"title\"/></p:nvPr></p:nvSpPr>"
  "<p:spPr/><p:txBody><a:bodyPr/><a:lstStyle/><a:p><a:endParaRPr lang=\"en-US\"/></a:p></p:txBody></p:sp>"
  "<p:sp><p:nvSpPr><p:cNvPr id=\"3\" name=\"Content Placeholder 2\"/>"
  "<p:cNvSpPr><a:spLocks noGrp=\"1\"/></p:cNvSpPr>"
  "<p:nvPr><p:ph idx=\"1\"/></p:nvPr></p:nvSpPr>"
  "<p:spPr/><p:txBody><a:bodyPr/><a:lstStyle/><a:p><a:endParaRPr lang=\"en-US\"/></a:p></p:txBody></p:sp>"
  "</p:spTree></p:cSld><p:clrMapOvr><a:masterClrMapping/></p:clrMapOvr></p:sldLayout>";

/* The Title Slide: the title large and centred a little above the
 * middle, the subtitle under it without bullets. */
static const char *LAYOUT_TITLE_XML = XML_HEAD
  "<p:sldLayout" NS_ALL " type=\"title\" preserve=\"1\">"
  "<p:cSld name=\"Title Slide\"><p:spTree>" GROUP_PROPS
  "<p:sp><p:nvSpPr><p:cNvPr id=\"2\" name=\"Title 1\"/>"
  "<p:cNvSpPr><a:spLocks noGrp=\"1\"/></p:cNvSpPr><p:nvPr><p:ph type=\"ctrTitle\"/></p:nvPr></p:nvSpPr>"
  "<p:spPr><a:xfrm><a:off x=\"1524000\" y=\"1122363\"/><a:ext cx=\"9144000\" cy=\"2387600\"/></a:xfrm></p:spPr>"
  "<p:txBody><a:bodyPr anchor=\"b\"><a:normAutofit/></a:bodyPr>"
  "<a:lstStyle><a:lvl1pPr algn=\"ctr\"><a:defRPr sz=\"6000\"/></a:lvl1pPr></a:lstStyle>"
  "<a:p><a:endParaRPr lang=\"en-US\"/></a:p></p:txBody></p:sp>"
  "<p:sp><p:nvSpPr><p:cNvPr id=\"3\" name=\"Subtitle 2\"/>"
  "<p:cNvSpPr><a:spLocks noGrp=\"1\"/></p:cNvSpPr>"
  "<p:nvPr><p:ph type=\"subTitle\" idx=\"1\"/></p:nvPr></p:nvSpPr>"
  "<p:spPr><a:xfrm><a:off x=\"1524000\" y=\"3602038\"/><a:ext cx=\"9144000\" cy=\"1655762\"/></a:xfrm></p:spPr>"
  "<p:txBody><a:bodyPr><a:normAutofit/></a:bodyPr>"
  "<a:lstStyle><a:lvl1pPr marL=\"0\" indent=\"0\" algn=\"ctr\"><a:buNone/>"
  "<a:defRPr sz=\"2400\"/></a:lvl1pPr></a:lstStyle>"
  "<a:p><a:endParaRPr lang=\"en-US\"/></a:p></p:txBody></p:sp>"
  "</p:spTree></p:cSld><p:clrMapOvr><a:masterClrMapping/></p:clrMapOvr></p:sldLayout>";

/* The notes page every notes slide hangs from: a picture of the slide
 * above, the notes below, on a portrait page. */
static const char *NOTES_MASTER_XML = XML_HEAD
  "<p:notesMaster" NS_ALL ">"
  "<p:cSld><p:bg><p:bgPr><a:solidFill><a:schemeClr val=\"bg1\"/></a:solidFill>"
  "<a:effectLst/></p:bgPr></p:bg><p:spTree>" GROUP_PROPS
  "<p:sp><p:nvSpPr><p:cNvPr id=\"2\" name=\"Slide Image Placeholder 1\"/>"
  "<p:cNvSpPr><a:spLocks noGrp=\"1\" noRot=\"1\" noChangeAspect=\"1\"/></p:cNvSpPr>"
  "<p:nvPr><p:ph type=\"sldImg\" idx=\"2\"/></p:nvPr></p:nvSpPr>"
  "<p:spPr><a:xfrm><a:off x=\"685800\" y=\"1143000\"/><a:ext cx=\"5486400\" cy=\"3086100\"/></a:xfrm>"
  "<a:prstGeom prst=\"rect\"><a:avLst/></a:prstGeom><a:noFill/>"
  "<a:ln w=\"12700\"><a:solidFill><a:srgbClr val=\"000000\"/></a:solidFill></a:ln></p:spPr></p:sp>"
  "<p:sp><p:nvSpPr><p:cNvPr id=\"3\" name=\"Notes Placeholder 2\"/>"
  "<p:cNvSpPr><a:spLocks noGrp=\"1\"/></p:cNvSpPr>"
  "<p:nvPr><p:ph type=\"body\" sz=\"quarter\" idx=\"3\"/></p:nvPr></p:nvSpPr>"
  "<p:spPr><a:xfrm><a:off x=\"685800\" y=\"4400550\"/><a:ext cx=\"5486400\" cy=\"3600450\"/></a:xfrm>"
  "<a:prstGeom prst=\"rect\"><a:avLst/></a:prstGeom></p:spPr>"
  "<p:txBody><a:bodyPr/><a:lstStyle/><a:p><a:endParaRPr lang=\"en-US\"/></a:p></p:txBody></p:sp>"
  "</p:spTree></p:cSld>" CLR_MAP
  "<p:notesStyle><a:lvl1pPr marL=\"0\" algn=\"l\"><a:defRPr sz=\"1200\">"
  "<a:solidFill><a:schemeClr val=\"tx1\"/></a:solidFill><a:latin typeface=\"+mn-lt\"/>"
  "</a:defRPr></a:lvl1pPr></p:notesStyle></p:notesMaster>";

/* The packages' fixed relationships.  Their lengths are strlen's to
 * work out: a count kept by hand goes stale the first time the text
 * changes, and then the zip writer reads past the string's end. */
static const char *ROOT_RELS = XML_HEAD
  "<Relationships xmlns=\"http://schemas.openxmlformats.org/package/2006/relationships\">"
  "<Relationship Id=\"rId1\" Type=\"" REL_NS "officeDocument\""
  " Target=\"ppt/presentation.xml\"/></Relationships>";

static const char *MASTER_RELS = XML_HEAD
  "<Relationships xmlns=\"http://schemas.openxmlformats.org/package/2006/relationships\">"
  "<Relationship Id=\"rId1\" Type=\"" REL_NS "slideLayout\" Target=\"../slideLayouts/slideLayout1.xml\"/>"
  "<Relationship Id=\"rId2\" Type=\"" REL_NS "slideLayout\" Target=\"../slideLayouts/slideLayout2.xml\"/>"
  "<Relationship Id=\"rId3\" Type=\"" REL_NS "theme\" Target=\"../theme/theme1.xml\"/>"
  "</Relationships>";

static const char *LAYOUT_RELS = XML_HEAD
  "<Relationships xmlns=\"http://schemas.openxmlformats.org/package/2006/relationships\">"
  "<Relationship Id=\"rId1\" Type=\"" REL_NS "slideMaster\""
  " Target=\"../slideMasters/slideMaster1.xml\"/></Relationships>";

static const char *NOTES_MASTER_RELS = XML_HEAD
  "<Relationships xmlns=\"http://schemas.openxmlformats.org/package/2006/relationships\">"
  "<Relationship Id=\"rId1\" Type=\"" REL_NS "theme\" Target=\"../theme/theme2.xml\"/>"
  "</Relationships>";

#define CT_PML "application/vnd.openxmlformats-officedocument.presentationml."

static void
zip_add_string (W42ZipWriter *zip, const char *name, const char *text)
{
  w42_zip_writer_add (zip, name, text, strlen (text));
}

gboolean
w42_pptx_save (W42PieceTable      *pt,
               const W42PageSetup *page,
               GFile              *file,
               GError            **error)
{
  W42ZipWriter *zip;
  GPtrArray *slides;
  GHashTable *media_types;       /* extension -> MIME type, for [Content_Types] */
  GString *s;
  gboolean ok, any_notes = FALSE;
  guint n_media = 0;

  g_return_val_if_fail (pt != NULL, FALSE);
  g_return_val_if_fail (G_IS_FILE (file), FALSE);

  (void) page;

  slides = w42_slides_from_document (pt);
  if (slides->len == 0)
    g_ptr_array_add (slides, slide_new ("", 1));
  for (guint i = 0; i < slides->len; i++)
    if (((W42Slide *) g_ptr_array_index (slides, i))->notes != NULL)
      any_notes = TRUE;

  zip = w42_zip_writer_new ();
  media_types = g_hash_table_new (g_str_hash, g_str_equal);

  /* The slides, their pictures and their notes first: the content types
   * and the relationships need to know what went in. */
  for (guint i = 0; i < slides->len; i++)
    {
      const W42Slide *slide = g_ptr_array_index (slides, i);
      char *name = g_strdup_printf ("ppt/slides/slide%u.xml", i + 1);
      char *rels_name = g_strdup_printf ("ppt/slides/_rels/slide%u.xml.rels", i + 1);
      char *xml = slide_xml (slide);
      GString *rels = g_string_new (XML_HEAD);

      g_string_append_printf (rels,
        "<Relationships xmlns=\"http://schemas.openxmlformats.org/package/2006/relationships\">"
        "<Relationship Id=\"rId1\" Type=\"" REL_NS "slideLayout\""
        " Target=\"../slideLayouts/slideLayout%d.xml\"/>", uses_title_layout (slide) ? 2 : 1);

      for (guint k = 0; k < slide->pictures->len && !uses_title_layout (slide); k++)
        {
          const W42SlidePicture *pic = g_ptr_array_index (slide->pictures, k);
          const char *ext = NULL, *mime = NULL;
          GBytes *bytes = w42_image_for_container (pic->data, &ext, &mime);
          char *media;

          if (bytes == NULL)
            {
              /* Unreadable: the relationship still has to exist for the
               * slide's reference, so it points at an empty picture. */
              bytes = g_bytes_new_static ("", 0);
              ext = "png";
              mime = "image/png";
            }
          media = g_strdup_printf ("ppt/media/image%u.%s", ++n_media, ext);
          w42_zip_writer_add (zip, media, g_bytes_get_data (bytes, NULL), g_bytes_get_size (bytes));
          g_hash_table_insert (media_types, (gpointer) ext, (gpointer) mime);
          g_string_append_printf (rels,
            "<Relationship Id=\"rId%u\" Type=\"" REL_NS "image\" Target=\"../media/image%u.%s\"/>",
            k + 2, n_media, ext);
          g_bytes_unref (bytes);
          g_free (media);
        }

      if (slide->notes != NULL)
        {
          char *notes_name = g_strdup_printf ("ppt/notesSlides/notesSlide%u.xml", i + 1);
          char *notes_rels = g_strdup_printf ("ppt/notesSlides/_rels/notesSlide%u.xml.rels", i + 1);
          char *nx = notes_xml (slide->notes);
          char *nr = g_strdup_printf (XML_HEAD
            "<Relationships xmlns=\"http://schemas.openxmlformats.org/package/2006/relationships\">"
            "<Relationship Id=\"rId1\" Type=\"" REL_NS "notesMaster\""
            " Target=\"../notesMasters/notesMaster1.xml\"/>"
            "<Relationship Id=\"rId2\" Type=\"" REL_NS "slide\" Target=\"../slides/slide%u.xml\"/>"
            "</Relationships>", i + 1);

          zip_add_string (zip, notes_name, nx);
          zip_add_string (zip, notes_rels, nr);
          g_string_append_printf (rels,
            "<Relationship Id=\"rId%u\" Type=\"" REL_NS "notesSlide\""
            " Target=\"../notesSlides/notesSlide%u.xml\"/>", slide->pictures->len + 2, i + 1);
          g_free (nr);
          g_free (nx);
          g_free (notes_rels);
          g_free (notes_name);
        }

      g_string_append (rels, "</Relationships>");
      zip_add_string (zip, name, xml);
      w42_zip_writer_add (zip, rels_name, rels->str, rels->len);
      g_string_free (rels, TRUE);
      g_free (xml);
      g_free (rels_name);
      g_free (name);
    }

  /* [Content_Types].xml */
  s = g_string_new (XML_HEAD);
  g_string_append (s,
    "<Types xmlns=\"http://schemas.openxmlformats.org/package/2006/content-types\">"
    "<Default Extension=\"rels\" ContentType=\"application/vnd.openxmlformats-package.relationships+xml\"/>"
    "<Default Extension=\"xml\" ContentType=\"application/xml\"/>");
  {
    GHashTableIter iter;
    gpointer ext, mime;

    g_hash_table_iter_init (&iter, media_types);
    while (g_hash_table_iter_next (&iter, &ext, &mime))
      g_string_append_printf (s, "<Default Extension=\"%s\" ContentType=\"%s\"/>",
                              (const char *) ext, (const char *) mime);
  }
  g_string_append (s,
    "<Override PartName=\"/ppt/presentation.xml\" ContentType=\"" CT_PML "presentation.main+xml\"/>"
    "<Override PartName=\"/ppt/slideMasters/slideMaster1.xml\" ContentType=\"" CT_PML "slideMaster+xml\"/>"
    "<Override PartName=\"/ppt/slideLayouts/slideLayout1.xml\" ContentType=\"" CT_PML "slideLayout+xml\"/>"
    "<Override PartName=\"/ppt/slideLayouts/slideLayout2.xml\" ContentType=\"" CT_PML "slideLayout+xml\"/>"
    "<Override PartName=\"/ppt/theme/theme1.xml\" ContentType=\"application/vnd.openxmlformats-officedocument.theme+xml\"/>");
  if (any_notes)
    g_string_append (s,
      "<Override PartName=\"/ppt/notesMasters/notesMaster1.xml\" ContentType=\"" CT_PML "notesMaster+xml\"/>"
      "<Override PartName=\"/ppt/theme/theme2.xml\" ContentType=\"application/vnd.openxmlformats-officedocument.theme+xml\"/>");
  for (guint i = 0; i < slides->len; i++)
    {
      g_string_append_printf (s,
        "<Override PartName=\"/ppt/slides/slide%u.xml\" ContentType=\"" CT_PML "slide+xml\"/>", i + 1);
      if (((W42Slide *) g_ptr_array_index (slides, i))->notes != NULL)
        g_string_append_printf (s,
          "<Override PartName=\"/ppt/notesSlides/notesSlide%u.xml\" ContentType=\"" CT_PML "notesSlide+xml\"/>",
          i + 1);
    }
  g_string_append (s, "</Types>");
  w42_zip_writer_add (zip, "[Content_Types].xml", s->str, s->len);
  g_string_free (s, TRUE);

  zip_add_string (zip, "_rels/.rels", ROOT_RELS);

  /* ppt/presentation.xml: the master, the notes master when there are
   * notes, the slides in order, and the 16:9 stage. */
  s = g_string_new (XML_HEAD);
  g_string_append (s,
    "<p:presentation" NS_ALL " saveSubsetFonts=\"1\">"
    "<p:sldMasterIdLst><p:sldMasterId id=\"2147483648\" r:id=\"rId1\"/></p:sldMasterIdLst>");
  if (any_notes)
    g_string_append_printf (s, "<p:notesMasterIdLst><p:notesMasterId r:id=\"rId%u\"/></p:notesMasterIdLst>",
                            slides->len + 3);
  g_string_append (s, "<p:sldIdLst>");
  for (guint i = 0; i < slides->len; i++)
    g_string_append_printf (s, "<p:sldId id=\"%u\" r:id=\"rId%u\"/>", 256 + i, i + 2);
  g_string_append_printf (s,
    "</p:sldIdLst>"
    "<p:sldSz cx=\"%d\" cy=\"%d\"/>"
    "<p:notesSz cx=\"6858000\" cy=\"9144000\"/></p:presentation>", SLIDE_W, SLIDE_H);
  w42_zip_writer_add (zip, "ppt/presentation.xml", s->str, s->len);
  g_string_free (s, TRUE);

  /* ppt/_rels/presentation.xml.rels */
  s = g_string_new (XML_HEAD);
  g_string_append (s,
    "<Relationships xmlns=\"http://schemas.openxmlformats.org/package/2006/relationships\">"
    "<Relationship Id=\"rId1\" Type=\"" REL_NS "slideMaster\" Target=\"slideMasters/slideMaster1.xml\"/>");
  for (guint i = 0; i < slides->len; i++)
    g_string_append_printf (s,
      "<Relationship Id=\"rId%u\" Type=\"" REL_NS "slide\" Target=\"slides/slide%u.xml\"/>", i + 2, i + 1);
  g_string_append_printf (s,
    "<Relationship Id=\"rId%u\" Type=\"" REL_NS "theme\" Target=\"theme/theme1.xml\"/>", slides->len + 2);
  if (any_notes)
    g_string_append_printf (s,
      "<Relationship Id=\"rId%u\" Type=\"" REL_NS "notesMaster\" Target=\"notesMasters/notesMaster1.xml\"/>",
      slides->len + 3);
  g_string_append (s, "</Relationships>");
  w42_zip_writer_add (zip, "ppt/_rels/presentation.xml.rels", s->str, s->len);
  g_string_free (s, TRUE);

  /* The master, its layouts and the theme; the notes master and its own
   * copy of the theme, which the format wants it to have. */
  zip_add_string (zip, "ppt/slideMasters/slideMaster1.xml", MASTER_XML);
  zip_add_string (zip, "ppt/slideMasters/_rels/slideMaster1.xml.rels", MASTER_RELS);
  zip_add_string (zip, "ppt/slideLayouts/slideLayout1.xml", LAYOUT_CONTENT_XML);
  zip_add_string (zip, "ppt/slideLayouts/_rels/slideLayout1.xml.rels", LAYOUT_RELS);
  zip_add_string (zip, "ppt/slideLayouts/slideLayout2.xml", LAYOUT_TITLE_XML);
  zip_add_string (zip, "ppt/slideLayouts/_rels/slideLayout2.xml.rels", LAYOUT_RELS);
  zip_add_string (zip, "ppt/theme/theme1.xml", THEME_XML);
  if (any_notes)
    {
      zip_add_string (zip, "ppt/notesMasters/notesMaster1.xml", NOTES_MASTER_XML);
      zip_add_string (zip, "ppt/notesMasters/_rels/notesMaster1.xml.rels", NOTES_MASTER_RELS);
      zip_add_string (zip, "ppt/theme/theme2.xml", THEME_XML);
    }

  ok = w42_zip_writer_save (zip, file, error);
  w42_zip_writer_free (zip);
  g_hash_table_destroy (media_types);
  w42_slides_free (slides);
  return ok;
}

/* ---------------------------------------------------------------------- */
/* Reading                                                                 */
/* ---------------------------------------------------------------------- */

/* What a shape is to the outline: the slide's title, its body -- whose
 * paragraphs are bullets unless they say otherwise -- some other text,
 * or a footer, a date or a slide number, which a document leaves out. */
typedef enum {
  SHAPE_OTHER,
  SHAPE_TITLE,
  SHAPE_CENTRED_TITLE,
  SHAPE_BODY,
  SHAPE_SKIP
} ShapeKind;

typedef struct {
  char    *text;
  int      level;
  gboolean no_bullet;      /* <a:buNone/> */
  gboolean bullet;         /* <a:buChar/> */
  gboolean numbered;       /* <a:buAutoNum/> */
} ReadPara;

typedef struct {
  ShapeKind  kind;
  GPtrArray *paras;        /* ReadPara * */
} ReadShape;

typedef struct {
  GString   *text;         /* the paragraph being read */
  ReadPara   para;         /* and what its properties said */
  gboolean   in_text;      /* inside <a:t> */
  gboolean   in_para;
  int        depth;        /* shapes open: a table's frame holds none,
                            * but a group's shapes are shapes of their own */
  int        in_pic;
  ReadShape *shape;        /* the shape being read */
  GPtrArray *shapes;       /* ReadShape *, in order */
  GPtrArray *pictures;     /* char *: the relationship ids of its pictures */
} SlideReader;

static void
read_para_free (gpointer data)
{
  ReadPara *p = data;

  g_free (p->text);
  g_free (p);
}

static void
read_shape_free (gpointer data)
{
  ReadShape *shape = data;

  g_ptr_array_free (shape->paras, TRUE);
  g_free (shape);
}

static const char *
local_name (const char *name)
{
  const char *colon = strrchr (name, ':');

  return colon != NULL ? colon + 1 : name;
}

static const char *
attr_value (const char **an, const char **av, const char *want)
{
  for (guint i = 0; an != NULL && an[i] != NULL; i++)
    if (g_str_equal (local_name (an[i]), want))
      return av[i];
  return NULL;
}

static void
slide_start (GMarkupParseContext *ctx, const char *name, const char **an,
             const char **av, gpointer data, GError **error)
{
  SlideReader *r = data;
  const char *tag = local_name (name);

  (void) ctx; (void) error;

  if (g_str_equal (tag, "sp") || g_str_equal (tag, "graphicFrame"))
    {
      if (r->depth++ == 0)
        {
          r->shape = g_new0 (ReadShape, 1);
          r->shape->kind = SHAPE_OTHER;
          r->shape->paras = g_ptr_array_new_with_free_func (read_para_free);
        }
    }
  else if (g_str_equal (tag, "pic"))
    r->in_pic++;
  else if (g_str_equal (tag, "blip") && r->in_pic > 0)
    {
      const char *id = attr_value (an, av, "embed");

      if (id != NULL)
        g_ptr_array_add (r->pictures, g_strdup (id));
    }
  else if (g_str_equal (tag, "ph") && r->shape != NULL)
    {
      const char *type = attr_value (an, av, "type");

      if (type == NULL || g_str_equal (type, "body") || g_str_equal (type, "obj"))
        r->shape->kind = SHAPE_BODY;
      else if (g_str_equal (type, "title"))
        r->shape->kind = SHAPE_TITLE;
      else if (g_str_equal (type, "ctrTitle"))
        r->shape->kind = SHAPE_CENTRED_TITLE;
      else if (g_str_equal (type, "dt") || g_str_equal (type, "ftr") ||
               g_str_equal (type, "sldNum") || g_str_equal (type, "sldImg") ||
               g_str_equal (type, "hdr"))
        r->shape->kind = SHAPE_SKIP;
    }
  else if (g_str_equal (tag, "p") && r->shape != NULL)
    {
      memset (&r->para, 0, sizeof r->para);
      g_string_truncate (r->text, 0);
      r->in_para = TRUE;
    }
  else if (g_str_equal (tag, "pPr") && r->in_para)
    {
      const char *lvl = attr_value (an, av, "lvl");

      if (lvl != NULL)
        r->para.level = CLAMP (atoi (lvl), 0, 8);
    }
  else if (g_str_equal (tag, "buNone") && r->in_para)
    r->para.no_bullet = TRUE;
  else if (g_str_equal (tag, "buChar") && r->in_para)
    r->para.bullet = TRUE;
  else if (g_str_equal (tag, "buAutoNum") && r->in_para)
    r->para.numbered = TRUE;
  else if (g_str_equal (tag, "t"))
    r->in_text = TRUE;
  else if (g_str_equal (tag, "br"))
    g_string_append_c (r->text, ' ');
}

static void
slide_end (GMarkupParseContext *ctx, const char *name, gpointer data, GError **error)
{
  SlideReader *r = data;
  const char *tag = local_name (name);

  (void) ctx; (void) error;

  if (g_str_equal (tag, "t"))
    r->in_text = FALSE;
  else if (g_str_equal (tag, "pic"))
    r->in_pic--;
  else if (g_str_equal (tag, "p") && r->in_para)
    {
      ReadPara *p = g_new0 (ReadPara, 1);

      *p = r->para;
      p->text = g_strdup (g_strstrip (r->text->str));
      g_ptr_array_add (r->shape->paras, p);
      g_string_truncate (r->text, 0);
      r->in_para = FALSE;
    }
  else if ((g_str_equal (tag, "sp") || g_str_equal (tag, "graphicFrame")) && r->depth > 0)
    {
      if (--r->depth == 0)
        {
          g_ptr_array_add (r->shapes, r->shape);
          r->shape = NULL;
        }
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

/* Reads one slide's (or notes page's) shapes and picture references. */
static void
read_slide_part (W42Zip *zip, const char *name, SlideReader *r)
{
  GBytes *bytes = w42_zip_read (zip, name);
  GMarkupParser parser = { slide_start, slide_end, slide_text, NULL, NULL };
  GMarkupParseContext *ctx;
  gsize len = 0;
  const char *d;

  memset (r, 0, sizeof *r);
  r->text = g_string_new (NULL);
  r->shapes = g_ptr_array_new_with_free_func (read_shape_free);
  r->pictures = g_ptr_array_new_with_free_func (g_free);
  if (bytes == NULL)
    return;

  d = g_bytes_get_data (bytes, &len);
  ctx = g_markup_parse_context_new (&parser, 0, r, NULL);
  if (g_markup_parse_context_parse (ctx, d, len, NULL))
    g_markup_parse_context_end_parse (ctx, NULL);
  g_markup_parse_context_free (ctx);
  g_bytes_unref (bytes);

  /* A part cut off in the middle of a shape: what was read of it counts. */
  if (r->shape != NULL)
    {
      g_ptr_array_add (r->shapes, r->shape);
      r->shape = NULL;
    }
}

static void
slide_reader_clear (SlideReader *r)
{
  g_string_free (r->text, TRUE);
  g_ptr_array_free (r->shapes, TRUE);
  g_ptr_array_free (r->pictures, TRUE);
  if (r->shape != NULL)
    read_shape_free (r->shape);
}

/* A part's name joined to a target relative to it: "ppt/slides/" and
 * "../media/image1.png" make "ppt/media/image1.png".  A target starting
 * with a slash is the package's own path. */
static char *
resolve_target (const char *part, const char *target)
{
  GPtrArray *segs = g_ptr_array_new_with_free_func (g_free);
  char **base, **rel;
  GString *out;

  if (target[0] == '/')
    return g_strdup (target + 1);

  base = g_strsplit (part, "/", -1);
  for (guint i = 0; base[i] != NULL && base[i + 1] != NULL; i++)   /* the directories */
    g_ptr_array_add (segs, g_strdup (base[i]));
  rel = g_strsplit (target, "/", -1);
  for (guint i = 0; rel[i] != NULL; i++)
    {
      if (g_str_equal (rel[i], ".."))
        {
          if (segs->len > 0)
            g_ptr_array_remove_index (segs, segs->len - 1);
        }
      else if (*rel[i] != '\0' && !g_str_equal (rel[i], "."))
        g_ptr_array_add (segs, g_strdup (rel[i]));
    }

  out = g_string_new (NULL);
  for (guint i = 0; i < segs->len; i++)
    {
      if (i > 0)
        g_string_append_c (out, '/');
      g_string_append (out, g_ptr_array_index (segs, i));
    }
  g_strfreev (base);
  g_strfreev (rel);
  g_ptr_array_free (segs, TRUE);
  return g_string_free (out, FALSE);
}

/* A part's relationships: id -> the target's name in the package.  The
 * notes page, if any, is filed under "notesSlide". */
static GHashTable *
read_part_rels (W42Zip *zip, const char *part)
{
  GHashTable *by_id = g_hash_table_new_full (g_str_hash, g_str_equal, g_free, g_free);
  const char *slash = strrchr (part, '/');
  char *rels_name = slash != NULL
    ? g_strdup_printf ("%.*s/_rels/%s.rels", (int) (slash - part), part, slash + 1)
    : g_strdup_printf ("_rels/%s.rels", part);
  GBytes *rels = w42_zip_read (zip, rels_name);
  gsize len = 0;
  const char *raw;
  char *d;
  const char *p;

  g_free (rels_name);
  if (rels == NULL)
    return by_id;

  raw = g_bytes_get_data (rels, &len);
  d = g_strndup (raw, len);
  p = d;
  while ((p = strstr (p, "<Relationship ")) != NULL)
    {
      const char *gt = strchr (p, '>');
      char *id = NULL, *target = NULL, *type = NULL;
      const char *q;

      if (gt == NULL)
        break;
      if ((q = g_strstr_len (p, gt - p, " Id=\"")) != NULL)
        {
          const char *e = strchr (q + 5, '"');
          if (e != NULL) id = g_strndup (q + 5, (gsize) (e - q - 5));
        }
      if ((q = g_strstr_len (p, gt - p, "Target=\"")) != NULL)
        {
          const char *e = strchr (q + 8, '"');
          if (e != NULL) target = g_strndup (q + 8, (gsize) (e - q - 8));
        }
      if ((q = g_strstr_len (p, gt - p, "Type=\"")) != NULL)
        {
          const char *e = strchr (q + 6, '"');
          if (e != NULL) type = g_strndup (q + 6, (gsize) (e - q - 6));
        }
      if (id != NULL && target != NULL && !g_str_has_prefix (target, "http"))
        {
          char *resolved = resolve_target (part, target);

          if (type != NULL && g_str_has_suffix (type, "/notesSlide"))
            g_hash_table_insert (by_id, g_strdup ("notesSlide"), g_strdup (resolved));
          g_hash_table_insert (by_id, id, resolved);
          id = NULL;
        }
      g_free (id);
      g_free (target);
      g_free (type);
      p = gt + 1;
    }
  g_free (d);
  g_bytes_unref (rels);
  return by_id;
}

/* The slide names in the order the presentation lists them. */
static GPtrArray *
slide_order (W42Zip *zip)
{
  GPtrArray *names = g_ptr_array_new_with_free_func (g_free);
  GHashTable *by_id = read_part_rels (zip, "ppt/presentation.xml");
  GBytes *pres = w42_zip_read (zip, "ppt/presentation.xml");

  if (pres != NULL)
    {
      gsize len = 0;
      const char *raw = g_bytes_get_data (pres, &len);
      /* A terminated copy: the zip's bytes carry no NUL of their own, so
       * strstr on them could run past the end of the entry. */
      char *d = g_strndup (raw, len);
      const char *list = strstr (d, "sldIdLst");
      const char *p = list != NULL ? list : d;

      /* Each slide once: a list that names one slide many times would
       * unpack it as many times. */
      GHashTable *seen = g_hash_table_new (g_str_hash, g_str_equal);

      while ((p = strstr (p, "r:id=\"")) != NULL)
        {
          const char *e = strchr (p + 6, '"');
          char *id;
          const char *target;

          if (e == NULL)
            break;
          id = g_strndup (p + 6, (gsize) (e - p - 6));
          target = g_hash_table_lookup (by_id, id);
          if (target != NULL && strstr (target, "slides/") != NULL &&
              !g_hash_table_contains (seen, target))
            {
              g_hash_table_add (seen, (gpointer) target);
              g_ptr_array_add (names, g_strdup (target));
            }
          g_free (id);
          p = e + 1;
        }
      g_hash_table_destroy (seen);
      g_free (d);
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

/* A title or a line of a slide as one paragraph's text: a line feed,
 * carriage return or form feed from an &#10; in the XML would end the
 * paragraph early, so they become spaces. */
static char *
one_line (const char *line)
{
  char *out = g_strdup (line);

  for (char *p = out; *p != '\0'; p++)
    if (*p == '\n' || *p == '\r' || *p == '\f' || *p == '\v')
      *p = ' ';
  return out;
}

/* The speaker's notes of a slide: the body text of its notes page. */
static char *
read_notes (W42Zip *zip, const char *notes_part)
{
  SlideReader r;
  GString *out = g_string_new (NULL);

  read_slide_part (zip, notes_part, &r);
  for (guint i = 0; i < r.shapes->len; i++)
    {
      const ReadShape *shape = g_ptr_array_index (r.shapes, i);

      if (shape->kind != SHAPE_BODY)
        continue;
      for (guint k = 0; k < shape->paras->len; k++)
        {
          const ReadPara *p = g_ptr_array_index (shape->paras, k);

          if (*p->text == '\0')
            continue;
          if (out->len > 0)
            g_string_append_c (out, '\n');
          g_string_append (out, p->text);
        }
    }
  slide_reader_clear (&r);
  return g_string_free (out, out->len == 0);
}

typedef struct {
  gsize       start;
  gsize       length;
  const char *style;       /* interned */
  const char *notes;       /* interned, or NULL */
} TitleMark;

/* One slide into the document: its title a heading, its text the
 * paragraphs under it, bulleted and levelled as the slide had them, its
 * pictures after them. */
static void
read_slide (W42Zip *zip, const char *name, W42Builder *b, GArray *titles)
{
  SlideReader r;
  GHashTable *rels = read_part_rels (zip, name);
  GString *title = g_string_new (NULL);
  gboolean centred = FALSE;
  TitleMark mark = { 0 };

  read_slide_part (zip, name, &r);

  for (guint i = 0; i < r.shapes->len; i++)
    {
      const ReadShape *shape = g_ptr_array_index (r.shapes, i);

      if (shape->kind != SHAPE_TITLE && shape->kind != SHAPE_CENTRED_TITLE)
        continue;
      centred |= shape->kind == SHAPE_CENTRED_TITLE;
      for (guint k = 0; k < shape->paras->len; k++)
        {
          const ReadPara *p = g_ptr_array_index (shape->paras, k);

          if (*p->text == '\0')
            continue;
          if (title->len > 0)
            g_string_append_c (title, ' ');
          g_string_append (title, p->text);
        }
    }

  {
    const char *notes_part = g_hash_table_lookup (rels, "notesSlide");
    char *notes = notes_part != NULL ? read_notes (zip, notes_part) : NULL;
    /* Translators: the heading given to a slide that has no title of its
     * own, when a presentation is opened. */
    char *text = one_line (title->len > 0 ? title->str : _("Slide"));

    mark.start = b->pos;
    w42_builder_text (b, text);
    mark.length = b->pos - mark.start;
    mark.style = g_intern_static_string (centred ? "Title" : "Heading 1");
    mark.notes = notes != NULL ? g_intern_string (notes) : NULL;
    g_array_append_val (titles, mark);
    w42_builder_end_paragraph (b);
    g_free (text);
    g_free (notes);
  }

  for (guint i = 0; i < r.shapes->len; i++)
    {
      const ReadShape *shape = g_ptr_array_index (r.shapes, i);

      if (shape->kind == SHAPE_TITLE || shape->kind == SHAPE_CENTRED_TITLE ||
          shape->kind == SHAPE_SKIP)
        continue;
      for (guint k = 0; k < shape->paras->len; k++)
        {
          const ReadPara *p = g_ptr_array_index (shape->paras, k);
          /* A body's paragraphs are bullets unless they say not; other
           * text -- a subtitle, a text box, a table -- only when they
           * say so. */
          gboolean bullet = !p->no_bullet &&
                            (shape->kind == SHAPE_BODY || p->bullet || p->numbered);
          char *text;

          if (*p->text == '\0')
            continue;
          text = one_line (p->text);
          if (bullet)
            {
              /* Indented as the Bullets button and Tab would have it: a
               * quarter inch a level, the bullet hanging in front. */
              b->pa.list = p->numbered ? W42_LIST_NUMBER : W42_LIST_BULLET;
              b->pa.list_level = (guint8) MIN (p->level, 8);
              b->pa.indent_left = 360 * (b->pa.list_level + 1);
              b->pa.indent_first = -360;
            }
          else if (p->level > 0)
            b->pa.indent_left = 720 * MIN (p->level, 8);
          w42_builder_text (b, text);
          w42_builder_end_paragraph (b);
          g_free (text);
        }
    }

  for (guint i = 0; i < r.pictures->len; i++)
    {
      const char *target = g_hash_table_lookup (rels, g_ptr_array_index (r.pictures, i));
      GBytes *bytes = target != NULL ? w42_zip_read (zip, target) : NULL;
      int width = 0, height = 0;
      const char *format = NULL;

      if (bytes != NULL && w42_image_probe (bytes, &width, &height, &format))
        {
          w42_builder_object (b, bytes, format, width, height, 0, 0);
          w42_builder_end_paragraph (b);
        }
      if (bytes != NULL)
        g_bytes_unref (bytes);
    }

  slide_reader_clear (&r);
  g_string_free (title, TRUE);
  g_hash_table_destroy (rels);
}

gboolean
w42_pptx_load (W42PieceTable *pt,
               W42PageSetup  *page,
               GFile         *file,
               GError       **error)
{
  W42Zip *zip;
  GPtrArray *names;
  GArray *titles;
  W42Builder b;

  g_return_val_if_fail (pt != NULL, FALSE);
  g_return_val_if_fail (G_IS_FILE (file), FALSE);

  zip = w42_zip_open (file, error);
  if (zip == NULL)
    return FALSE;

  if (!w42_zip_has (zip, "ppt/presentation.xml"))
    {
      w42_zip_free (zip);
      g_set_error (error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA,
                   /* Translators: ppt/presentation.xml is the name of a file
                    * inside a .pptx; keep it as it is. */
                   _("That file is not a presentation: it has no ppt/presentation.xml."));
      return FALSE;
    }

  if (page != NULL && page->width == 0)
    {
      page->width = 12240; page->height = 15840;
      page->margin_left = page->margin_right = 1440;
      page->margin_top = page->margin_bottom = 1440;
    }

  names = slide_order (zip);
  titles = g_array_new (FALSE, FALSE, sizeof (TitleMark));

  w42_builder_init (&b, pt);
  for (guint i = 0; i < names->len; i++)
    read_slide (zip, g_ptr_array_index (names, i), &b, titles);
  w42_builder_finish (&b);

  /* Each slide's title is a heading, which is what makes the document an
   * outline again -- and what lets it be shown as slides.  The notes go
   * on after the style, which would otherwise restyle them away. */
  for (guint i = 0; i < titles->len; i++)
    {
      const TitleMark *mark = &g_array_index (titles, TitleMark, i);

      w42_pt_apply_style (pt, mark->start, 1, mark->style);
      if (mark->notes != NULL && mark->length > 0)
        {
          W42CharFmt want;

          memset (&want, 0, sizeof want);
          want.comment = mark->notes;
          w42_pt_apply_char_fmt (pt, mark->start, mark->length, W42_CHAR_COMMENT, &want);
        }
    }

  g_array_free (titles, TRUE);
  g_ptr_array_free (names, TRUE);
  w42_zip_free (zip);
  return TRUE;
}
