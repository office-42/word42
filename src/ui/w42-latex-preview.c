/* w42-latex-preview.c - File > LaTeX Preview: the document as LaTeX sets it
 *
 * Copyright (C) 2026 Andreas Røsdal
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * The pane keeps a folder of its own to typeset in for as long as it
 * lives.  Each run writes the document there as document.tex, and runs
 * the engine only when the source differs from the one it last set:
 * moving the caret, or an edit undone, costs nothing.  The .aux and the
 * other files the engine leaves stay from one run to the next, so the
 * cross-references are right after one run where a fresh folder would
 * need two.  A run that fails leaves the last good pages showing, with
 * what LaTeX said above them.
 */

#include "w42-latex-preview.h"

#include <string.h>
#include <glib/gi18n.h>
#include <poppler.h>

#include "w42-document.h"
#include "w42-latex.h"
#include "w42-latex-run.h"
#include "w42-pdf-view.h"
#include "w42-settings.h"
#include "w42-synctex.h"

/* How long the typing has to pause before the document is set again, and
 * before the preview goes to where the caret has gone. */
#define TYPESET_DELAY_MS 1200
#define FOLLOW_DELAY_MS   350

/* A move of the caret further than this is a jump, and the place it
 * lands on is marked; typing along is followed without. */
#define JUMP_CHARS 40

typedef struct {
  GtkWidget    *root;
  W42View      *view;
  W42Document  *doc;
  gulong        doc_changed_id;

  GtkWidget    *pdf;            /* W42PdfView */
  GtkWidget    *source;         /* GtkTextView */
  GtkWidget    *stack;
  GtkWidget    *none;           /* what is shown when nothing can be */
  GtkWidget    *status;
  GtkWidget    *spinner;
  GtkWidget    *error_bar;
  GtkWidget    *error_label;
  GtkWidget    *zoom_label;
  GtkWidget    *auto_check;
  GtkWidget    *pages_btn, *source_btn;

  char         *dir;            /* the folder the typesetting is done in */
  char         *engine;
  gboolean      engine_looked;
  char         *last_source;    /* the source last sent to the engine */
  gboolean      last_ok;
  gboolean      stale;          /* the text changed while the pane was hidden */
  GArray       *anchors;        /* W42LatexAnchor, of the pages shown */
  GArray       *pending;        /* those of the source being set */
  GArray       *failed;         /* those of the source LaTeX stopped in */
  W42SyncTex   *sync;
  int           error_line;

  gboolean      running;
  gboolean      again;          /* the text changed while it was being set */
  guint         typeset_id;
  guint         follow_id;
  guint         zoom_id;        /* the zoom's label, to be brought up to date */
  gsize         followed;       /* where the caret was last followed to */
  gint64        started;
  GCancellable *cancellable;
} Preview;

static void preview_typeset (Preview *p, gboolean force);

static Preview *
preview_of (GtkWidget *root)
{
  return g_object_get_data (G_OBJECT (root), "w42-latex-preview");
}

static void
say (Preview *p, const char *text)
{
  gtk_label_set_text (GTK_LABEL (p->status), text != NULL ? text : "");
}

/* ---- the source and the document -------------------------------------- */

/* The line of the source a position in the document went to, from the
 * anchors of the pages shown; 0 when there is none. */
static guint
line_for_pos (Preview *p, gsize pos)
{
  guint lo = 0, hi;

  if (p->anchors == NULL || p->anchors->len == 0)
    return 0;
  hi = p->anchors->len;
  /* The last anchor at or before `pos`. */
  while (hi - lo > 1)
    {
      guint mid = (lo + hi) / 2;

      if (g_array_index (p->anchors, W42LatexAnchor, mid).pos <= pos)
        lo = mid;
      else
        hi = mid;
    }
  return g_array_index (p->anchors, W42LatexAnchor, lo).line;
}

/* The position in the document a line of the source came from: the
 * first anchor on the last line at or before it. */
static gboolean
pos_for_line (GArray *anchors, guint line, gsize *pos)
{
  guint best = 0;
  gboolean found = FALSE;

  if (anchors == NULL || line == 0)
    return FALSE;
  for (guint i = 0; i < anchors->len; i++)
    {
      const W42LatexAnchor *a = &g_array_index (anchors, W42LatexAnchor, i);

      if (a->line <= line && (!found || a->line > best || (a->line == best && a->pos < *pos)))
        {
          best = a->line;
          *pos = a->pos;
          found = TRUE;
        }
    }
  return found;
}

static void
go_to_pos (Preview *p, gsize pos)
{
  W42PieceTable *pt;

  if (p->view == NULL || p->doc == NULL)
    return;
  pt = w42_document_pt (p->doc);
  pos = w42_pt_clamp_pos (pt, pos);
  p->followed = pos;
  w42_view_select_range (p->view, pos, pos);
  gtk_widget_grab_focus (GTK_WIDGET (p->view));
}

static void
source_show_line (Preview *p, int line, gboolean select)
{
  GtkTextBuffer *buffer = gtk_text_view_get_buffer (GTK_TEXT_VIEW (p->source));
  GtkTextIter start, end;

  if (line < 1)
    return;
  gtk_text_buffer_get_iter_at_line (buffer, &start, line - 1);
  end = start;
  gtk_text_iter_forward_to_line_end (&end);
  if (select)
    gtk_text_buffer_select_range (buffer, &start, &end);
  else
    gtk_text_buffer_place_cursor (buffer, &start);
  gtk_text_view_scroll_to_iter (GTK_TEXT_VIEW (p->source), &start, 0.1, TRUE, 0.0, 0.3);
}

static void
source_mark_error (Preview *p, int line)
{
  GtkTextBuffer *buffer = gtk_text_view_get_buffer (GTK_TEXT_VIEW (p->source));
  GtkTextIter start, end;

  gtk_text_buffer_get_bounds (buffer, &start, &end);
  gtk_text_buffer_remove_tag_by_name (buffer, "error", &start, &end);
  if (line < 1)
    return;
  gtk_text_buffer_get_iter_at_line (buffer, &start, line - 1);
  end = start;
  gtk_text_iter_forward_line (&end);
  gtk_text_buffer_apply_tag_by_name (buffer, "error", &start, &end);
}

/* ---- following the caret ---------------------------------------------- */

static void
preview_follow (Preview *p, gboolean mark)
{
  gsize caret;
  guint line;
  int page;
  double top, bottom;

  if (p->view == NULL || p->sync == NULL)
    return;
  caret = w42_view_get_caret (p->view);
  line = line_for_pos (p, caret);
  if (line > 0 && w42_synctex_forward (p->sync, line, &page, &top, &bottom))
    w42_pdf_view_show (W42_PDF_VIEW (p->pdf), page, top, bottom, mark);
  p->followed = caret;
}

static gboolean
on_follow (gpointer data)
{
  Preview *p = data;
  gsize caret = p->view != NULL ? w42_view_get_caret (p->view) : 0;
  gsize moved = caret > p->followed ? caret - p->followed : p->followed - caret;

  p->follow_id = 0;
  if (gtk_widget_get_visible (p->root))
    preview_follow (p, moved > JUMP_CHARS);
  return G_SOURCE_REMOVE;
}

/* A double-click on a page: the caret to what was clicked. */
static void
on_point_activated (W42PdfView *view, int page, double x, double y, gpointer data)
{
  Preview *p = data;
  guint line = p->sync != NULL ? w42_synctex_inverse (p->sync, page, x, y) : 0;
  gsize pos = 0;

  if (pos_for_line (p->anchors, line, &pos))
    go_to_pos (p, pos);
}

/* And a double-click on a line of the source. */
static void
on_source_pressed (GtkGestureClick *gesture, int n_press, double x, double y, gpointer data)
{
  Preview *p = data;
  GtkTextIter iter;
  int bx, by;
  gsize pos = 0;

  if (n_press != 2)
    return;
  gtk_text_view_window_to_buffer_coords (GTK_TEXT_VIEW (p->source), GTK_TEXT_WINDOW_WIDGET,
                                         (int) x, (int) y, &bx, &by);
  gtk_text_view_get_iter_at_location (GTK_TEXT_VIEW (p->source), &iter, bx, by);
  if (pos_for_line (p->last_ok ? p->anchors : p->failed, (guint) gtk_text_iter_get_line (&iter) + 1, &pos))
    go_to_pos (p, pos);
}

/* ---- typesetting ------------------------------------------------------ */

static void
show_none (Preview *p, const char *text)
{
  gtk_label_set_text (GTK_LABEL (p->none), text);
  gtk_stack_set_visible_child_name (GTK_STACK (p->stack), "none");
}

static void
show_pages_or_source (Preview *p)
{
  gtk_stack_set_visible_child_name (GTK_STACK (p->stack),
                                    gtk_toggle_button_get_active (GTK_TOGGLE_BUTTON (p->source_btn))
                                    ? "source" : "pages");
}

static gboolean
update_zoom_label (gpointer data)
{
  Preview *p = data;
  char *text = g_strdup_printf ("%d%%", (int) (w42_pdf_view_get_scale (W42_PDF_VIEW (p->pdf)) * 100 + 0.5));

  p->zoom_id = 0;
  gtk_label_set_text (GTK_LABEL (p->zoom_label), text);
  g_free (text);
  return G_SOURCE_REMOVE;
}

/* The scale is worked out when the pages are next laid out, so the label
 * waits for that. */
static void
on_scale_changed (GtkWidget *pdf, gpointer data)
{
  Preview *p = data;

  if (p->zoom_id == 0)
    p->zoom_id = g_idle_add (update_zoom_label, p);
}

static void
on_typeset (const W42LatexResult *result, gpointer data)
{
  Preview *p = data;
  double seconds = (g_get_monotonic_time () - p->started) / 1e6;

  p->running = FALSE;
  gtk_spinner_stop (GTK_SPINNER (p->spinner));
  gtk_widget_set_visible (p->spinner, FALSE);

  if (result->ok)
    {
      GFile *file = g_file_new_for_path (result->pdf);
      GBytes *bytes = g_file_load_bytes (file, NULL, NULL, NULL);
      PopplerDocument *pdf = NULL;
      GError *error = NULL;

      /* Read into memory: the next run writes over the file. */
      if (bytes != NULL)
        pdf = poppler_document_new_from_bytes (bytes, NULL, &error);
      if (pdf != NULL)
        {
          int n = poppler_document_get_n_pages (pdf);
          char *text;

          w42_pdf_view_set_document (W42_PDF_VIEW (p->pdf), pdf);
          g_object_unref (pdf);
          g_clear_pointer (&p->sync, w42_synctex_free);
          if (result->synctex != NULL)
            p->sync = w42_synctex_load (result->synctex, "document.tex", NULL);
          g_clear_pointer (&p->anchors, g_array_unref);
          g_clear_pointer (&p->failed, g_array_unref);
          p->anchors = g_steal_pointer (&p->pending);
          p->last_ok = TRUE;
          p->error_line = 0;
          gtk_widget_set_visible (p->error_bar, FALSE);
          source_mark_error (p, 0);
          show_pages_or_source (p);
          on_scale_changed (p->pdf, p);
          /* Translators: the LaTeX preview's status: the pages made, the
           * engine that made them (as "pdflatex") and the seconds it took. */
          text = g_strdup_printf (ngettext ("%d page, set by %s in %.1f s.",
                                            "%d pages, set by %s in %.1f s.", (unsigned long) n),
                                  n, result->engine, seconds);
          say (p, text);
          g_free (text);
          /* What was typed stays in sight as the pages it is on grow. */
          preview_follow (p, FALSE);
        }
      else
        {
          p->last_ok = FALSE;
          say (p, error != NULL ? error->message : _("Word42 could not read the PDF LaTeX made."));
        }
      g_clear_error (&error);
      if (bytes != NULL)
        g_bytes_unref (bytes);
      g_object_unref (file);
    }
  else
    {
      char *first, *nl, *text;

      p->last_ok = FALSE;
      g_clear_pointer (&p->failed, g_array_unref);
      p->failed = g_steal_pointer (&p->pending);
      p->error_line = result->line;
      source_mark_error (p, result->line);
      first = g_strdup (result->errors != NULL ? result->errors : "");
      g_strstrip (first);
      /* What LaTeX said, a few lines of it. */
      nl = first;
      for (int k = 0; k < 3 && nl != NULL; k++)
        {
          nl = strchr (nl, '\n');
          if (nl != NULL)
            nl++;
        }
      if (nl != NULL)
        nl[-1] = '\0';
      if (result->line > 0)
        /* Translators: %d is the line of the LaTeX source the error is
         * on, %s what LaTeX said about it. */
        text = g_strdup_printf (_("LaTeX stopped at line %d of the source:\n%s"), result->line, first);
      else
        /* Translators: %s is what LaTeX said went wrong. */
        text = g_strdup_printf (_("LaTeX stopped:\n%s"), first);
      gtk_label_set_text (GTK_LABEL (p->error_label), text);
      gtk_widget_set_visible (p->error_bar, TRUE);
      /* With no pages yet to show, the source, where the error is. */
      if (w42_pdf_view_get_n_pages (W42_PDF_VIEW (p->pdf)) == 0)
        {
          gtk_toggle_button_set_active (GTK_TOGGLE_BUTTON (p->source_btn), TRUE);
          show_pages_or_source (p);
          source_show_line (p, result->line, FALSE);
        }
      say (p, _("LaTeX stopped with an error. The pages shown are the last it set."));
      g_free (text);
      g_free (first);
    }

  if (p->again)
    {
      p->again = FALSE;
      preview_typeset (p, FALSE);
    }
}

static void
preview_typeset (Preview *p, gboolean force)
{
  GFile *tex;
  GArray *anchors = NULL;
  GError *error = NULL;
  char *source = NULL;

  if (p->typeset_id != 0)
    {
      g_source_remove (p->typeset_id);
      p->typeset_id = 0;
    }
  if (p->doc == NULL)
    return;
  if (!gtk_widget_get_visible (p->root))
    {
      p->stale = TRUE;
      return;
    }
  p->stale = FALSE;
  if (p->running)
    {
      p->again = TRUE;
      return;
    }

  /* Asked outright, a TeX installed since the last look is found. */
  if (!p->engine_looked || (force && p->engine == NULL))
    {
      p->engine = w42_latex_find_engine ();
      p->engine_looked = TRUE;
    }
  if (p->engine == NULL)
    {
      show_none (p, _("The preview needs a TeX engine, and none is installed.\n\n"
                      "Install Tectonic (tectonic-typesetting.github.io), which fetches what "
                      "it needs by itself, or MiKTeX (miktex.org) or TeX Live "
                      "(tug.org/texlive). Word42 looks for them on the PATH and where their "
                      "installers put them."));
      say (p, _("No TeX engine is installed."));
      return;
    }
  if (p->dir == NULL)
    {
      p->dir = g_dir_make_tmp ("word42-preview-XXXXXX", &error);
      if (p->dir == NULL)
        {
          say (p, error->message);
          g_error_free (error);
          return;
        }
    }

  tex = g_file_new_build_filename (p->dir, "document.tex", NULL);
  if (!w42_latex_export_anchored (w42_document_pt (p->doc), w42_document_page_setup (p->doc),
                                  tex, &anchors, &error) ||
      !g_file_load_contents (tex, NULL, &source, NULL, NULL, &error))
    {
      say (p, error->message);
      g_error_free (error);
      g_object_unref (tex);
      if (anchors != NULL)
        g_array_unref (anchors);
      return;
    }
  g_object_unref (tex);

  /* Nothing LaTeX would see has changed. */
  if (!force && p->last_ok && g_strcmp0 (source, p->last_source) == 0)
    {
      g_free (source);
      g_array_unref (anchors);
      return;
    }

  if (g_strcmp0 (source, p->last_source) != 0)
    {
      GtkTextBuffer *buffer = gtk_text_view_get_buffer (GTK_TEXT_VIEW (p->source));

      gtk_text_buffer_set_text (buffer, source, -1);
    }
  g_free (p->last_source);
  p->last_source = source;
  g_clear_pointer (&p->pending, g_array_unref);
  p->pending = anchors;

  p->running = TRUE;
  p->started = g_get_monotonic_time ();
  gtk_widget_set_visible (p->spinner, TRUE);
  gtk_spinner_start (GTK_SPINNER (p->spinner));
  {
    char *base = g_path_get_basename (p->engine);
    /* Translators: %s is the TeX engine's name, as "pdflatex". */
    char *text = g_strdup_printf (_("Setting the document with %s..."), base);

    say (p, text);
    g_free (text);
    g_free (base);
  }
  w42_latex_compile (p->engine, p->dir, TRUE, p->cancellable, on_typeset, p);
}

static gboolean
on_typeset_due (gpointer data)
{
  Preview *p = data;

  p->typeset_id = 0;
  preview_typeset (p, FALSE);
  return G_SOURCE_REMOVE;
}

static void
schedule_typeset (Preview *p, guint delay)
{
  if (p->typeset_id != 0)
    g_source_remove (p->typeset_id);
  p->typeset_id = g_timeout_add (delay, on_typeset_due, p);
}

/* ---- following the document ------------------------------------------- */

static void
on_document_changed (W42Document *doc, gpointer data)
{
  Preview *p = data;

  if (!gtk_widget_get_visible (p->root))
    p->stale = TRUE;
  else if (gtk_check_button_get_active (GTK_CHECK_BUTTON (p->auto_check)))
    schedule_typeset (p, TYPESET_DELAY_MS);
}

static void
preview_watch_document (Preview *p, W42Document *doc)
{
  if (p->doc == doc)
    return;
  if (p->doc != NULL && p->doc_changed_id != 0)
    g_signal_handler_disconnect (p->doc, p->doc_changed_id);
  p->doc_changed_id = 0;
  g_set_object (&p->doc, doc);
  /* Another document: nothing of the last one's is to be kept. */
  g_clear_pointer (&p->last_source, g_free);
  g_clear_pointer (&p->anchors, g_array_unref);
  g_clear_pointer (&p->sync, w42_synctex_free);
  p->last_ok = FALSE;
  p->followed = G_MAXSIZE;
  if (doc != NULL)
    {
      p->doc_changed_id = g_signal_connect (doc, "changed", G_CALLBACK (on_document_changed), p);
      schedule_typeset (p, 0);
    }
}

static void
on_view_state_changed (W42View *view, gpointer data)
{
  Preview *p = preview_of (data);

  preview_watch_document (p, w42_view_get_document (view));
  if (!gtk_widget_get_visible (p->root) || w42_view_get_caret (view) == p->followed)
    return;
  if (p->follow_id != 0)
    g_source_remove (p->follow_id);
  p->follow_id = g_timeout_add (FOLLOW_DELAY_MS, on_follow, p);
}

static void
on_visible (GObject *root, GParamSpec *pspec, gpointer data)
{
  Preview *p = data;

  if (gtk_widget_get_visible (p->root))
    {
      if (p->stale || !p->last_ok)
        schedule_typeset (p, 0);
    }
  else if (p->typeset_id != 0)
    {
      g_source_remove (p->typeset_id);
      p->typeset_id = 0;
      p->stale = TRUE;
    }
}

/* ---- the bar ---------------------------------------------------------- */

static void
on_typeset_clicked (GtkButton *button, gpointer data)
{
  preview_typeset (data, TRUE);
}

static void
on_auto_toggled (GtkCheckButton *check, gpointer data)
{
  Preview *p = data;
  gboolean on = gtk_check_button_get_active (check);

  w42_settings_set_bool ("latex-preview-auto", on);
  if (on)
    schedule_typeset (p, 0);
}

static void
on_zoom_clicked (GtkButton *button, gpointer data)
{
  Preview *p = data;
  W42PdfView *pdf = W42_PDF_VIEW (p->pdf);
  const char *which = g_object_get_data (G_OBJECT (button), "w42-zoom");
  double now = w42_pdf_view_get_scale (pdf);

  if (g_str_equal (which, "fit"))
    w42_pdf_view_set_zoom (pdf, 0);
  else
    w42_pdf_view_set_zoom (pdf, g_str_equal (which, "in") ? now * 1.25 : now / 1.25);
}

static void
on_which_toggled (GtkToggleButton *button, gpointer data)
{
  Preview *p = data;

  if (gtk_toggle_button_get_active (button) &&
      !g_str_equal (gtk_stack_get_visible_child_name (GTK_STACK (p->stack)), "none"))
    show_pages_or_source (p);
}

static void
on_open_clicked (GtkButton *button, gpointer data)
{
  Preview *p = data;
  char *path = p->dir != NULL ? g_build_filename (p->dir, "document.pdf", NULL) : NULL;

  if (path != NULL && g_file_test (path, G_FILE_TEST_EXISTS))
    {
      GFile *file = g_file_new_for_path (path);
      GtkFileLauncher *launcher = gtk_file_launcher_new (file);
      GtkRoot *root = gtk_widget_get_root (p->root);

      gtk_file_launcher_launch (launcher, GTK_IS_WINDOW (root) ? GTK_WINDOW (root) : NULL,
                                NULL, NULL, NULL);
      g_object_unref (launcher);
      g_object_unref (file);
    }
  g_free (path);
}

static void
on_show_source (GtkButton *button, gpointer data)
{
  Preview *p = data;

  gtk_toggle_button_set_active (GTK_TOGGLE_BUTTON (p->source_btn), TRUE);
  show_pages_or_source (p);
  source_show_line (p, p->error_line, TRUE);
}

static void
on_go_to_text (GtkButton *button, gpointer data)
{
  Preview *p = data;
  gsize pos = 0;

  if (pos_for_line (p->failed, (guint) MAX (p->error_line, 0), &pos))
    go_to_pos (p, pos);
}

static GtkWidget *
bar_button (GtkWidget *bar, const char *label, const char *tip, GCallback on_click, gpointer data)
{
  GtkWidget *button = gtk_button_new_with_label (label);

  gtk_widget_set_tooltip_text (button, tip);
  g_signal_connect (button, "clicked", on_click, data);
  gtk_box_append (GTK_BOX (bar), button);
  return button;
}

/* ---- the pane --------------------------------------------------------- */

/* Nothing more is started or waited for once the pane is going: the
 * engine is stopped, and what it would have said goes unheard. */
static void
on_destroy (GtkWidget *root, gpointer data)
{
  Preview *p = data;

  g_cancellable_cancel (p->cancellable);
  if (p->typeset_id != 0)
    g_source_remove (p->typeset_id);
  if (p->follow_id != 0)
    g_source_remove (p->follow_id);
  if (p->zoom_id != 0)
    g_source_remove (p->zoom_id);
  p->typeset_id = p->follow_id = p->zoom_id = 0;
  if (p->doc != NULL && p->doc_changed_id != 0)
    g_signal_handler_disconnect (p->doc, p->doc_changed_id);
  p->doc_changed_id = 0;
  w42_latex_preview_set_view (root, NULL);
}

static void
preview_free (gpointer data)
{
  Preview *p = data;

  g_cancellable_cancel (p->cancellable);
  g_clear_object (&p->cancellable);
  if (p->typeset_id != 0)
    g_source_remove (p->typeset_id);
  if (p->follow_id != 0)
    g_source_remove (p->follow_id);
  if (p->zoom_id != 0)
    g_source_remove (p->zoom_id);
  if (p->doc != NULL && p->doc_changed_id != 0)
    g_signal_handler_disconnect (p->doc, p->doc_changed_id);
  if (p->view != NULL)
    g_object_remove_weak_pointer (G_OBJECT (p->view), (gpointer *) &p->view);
  g_clear_object (&p->doc);
  g_clear_pointer (&p->sync, w42_synctex_free);
  g_clear_pointer (&p->anchors, g_array_unref);
  g_clear_pointer (&p->pending, g_array_unref);
  g_clear_pointer (&p->failed, g_array_unref);
  /* The engine is stopped by now; what it left goes with the pane. */
  w42_latex_remove_dir (p->dir);
  g_free (p->dir);
  g_free (p->engine);
  g_free (p->last_source);
  g_free (p);
}

GtkWidget *
w42_latex_preview_new (W42View *view)
{
  Preview *p = g_new0 (Preview, 1);
  GtkWidget *bar, *scroller, *row, *button, *sep, *foot;
  GtkTextBuffer *buffer;
  GtkGesture *click;

  p->cancellable = g_cancellable_new ();
  p->followed = G_MAXSIZE;

  p->root = gtk_box_new (GTK_ORIENTATION_VERTICAL, 0);
  gtk_widget_add_css_class (p->root, "w42-latex-preview");
  gtk_widget_set_size_request (p->root, 200, -1);
  g_object_set_data_full (G_OBJECT (p->root), "w42-latex-preview", p, preview_free);

  /* The bar: set now, set as you type, the size, pages or source, and
   * the PDF in the desktop's own viewer. */
  bar = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 3);
  gtk_widget_add_css_class (bar, "w42-toolbar");
  gtk_widget_set_margin_start (bar, 3);
  gtk_widget_set_margin_end (bar, 3);
  gtk_widget_set_margin_top (bar, 2);
  gtk_widget_set_margin_bottom (bar, 2);
  bar_button (bar, _("Typeset"), _("Set the document with LaTeX now"),
              G_CALLBACK (on_typeset_clicked), p);
  p->auto_check = gtk_check_button_new_with_label (_("As you type"));
  gtk_widget_set_tooltip_text (p->auto_check, _("Set the document again whenever the typing pauses"));
  gtk_check_button_set_active (GTK_CHECK_BUTTON (p->auto_check),
                               w42_settings_get_bool ("latex-preview-auto", TRUE));
  g_signal_connect (p->auto_check, "toggled", G_CALLBACK (on_auto_toggled), p);
  gtk_box_append (GTK_BOX (bar), p->auto_check);
  sep = gtk_separator_new (GTK_ORIENTATION_VERTICAL);
  gtk_box_append (GTK_BOX (bar), sep);
  button = bar_button (bar, "\342\210\222", _("Zoom out"), G_CALLBACK (on_zoom_clicked), p);
  g_object_set_data (G_OBJECT (button), "w42-zoom", (gpointer) "out");
  p->zoom_label = gtk_label_new ("");
  gtk_label_set_width_chars (GTK_LABEL (p->zoom_label), 5);
  gtk_box_append (GTK_BOX (bar), p->zoom_label);
  button = bar_button (bar, "+", _("Zoom in"), G_CALLBACK (on_zoom_clicked), p);
  g_object_set_data (G_OBJECT (button), "w42-zoom", (gpointer) "in");
  /* Translators: the LaTeX preview's button that fits the page to the
   * pane's width; keep it short. */
  button = bar_button (bar, C_("zoom", "Fit"), _("Fit the page to the width"),
                       G_CALLBACK (on_zoom_clicked), p);
  g_object_set_data (G_OBJECT (button), "w42-zoom", (gpointer) "fit");
  sep = gtk_separator_new (GTK_ORIENTATION_VERTICAL);
  gtk_box_append (GTK_BOX (bar), sep);
  p->pages_btn = gtk_toggle_button_new_with_label (_("Pages"));
  gtk_widget_set_tooltip_text (p->pages_btn, _("The pages LaTeX set"));
  p->source_btn = gtk_toggle_button_new_with_label (_("Source"));
  gtk_widget_set_tooltip_text (p->source_btn, _("The LaTeX source they were set from"));
  gtk_toggle_button_set_group (GTK_TOGGLE_BUTTON (p->source_btn), GTK_TOGGLE_BUTTON (p->pages_btn));
  gtk_toggle_button_set_active (GTK_TOGGLE_BUTTON (p->pages_btn), TRUE);
  g_signal_connect (p->pages_btn, "toggled", G_CALLBACK (on_which_toggled), p);
  g_signal_connect (p->source_btn, "toggled", G_CALLBACK (on_which_toggled), p);
  gtk_box_append (GTK_BOX (bar), p->pages_btn);
  gtk_box_append (GTK_BOX (bar), p->source_btn);
  button = bar_button (bar, _("Open"), _("Open the PDF in the desktop's viewer"),
                       G_CALLBACK (on_open_clicked), p);
  gtk_widget_set_hexpand (button, TRUE);
  gtk_widget_set_halign (button, GTK_ALIGN_END);
  gtk_box_append (GTK_BOX (p->root), bar);

  /* What LaTeX said when it stopped, over the last pages it set. */
  p->error_bar = gtk_box_new (GTK_ORIENTATION_VERTICAL, 4);
  gtk_widget_add_css_class (p->error_bar, "w42-latex-error");
  p->error_label = gtk_label_new ("");
  gtk_label_set_xalign (GTK_LABEL (p->error_label), 0.0);
  gtk_label_set_wrap (GTK_LABEL (p->error_label), TRUE);
  gtk_label_set_wrap_mode (GTK_LABEL (p->error_label), PANGO_WRAP_WORD_CHAR);
  gtk_label_set_selectable (GTK_LABEL (p->error_label), TRUE);
  gtk_box_append (GTK_BOX (p->error_bar), p->error_label);
  row = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 4);
  gtk_widget_add_css_class (row, "w42-toolbar");
  bar_button (row, _("Show in Source"), _("The line of the source LaTeX stopped at"),
              G_CALLBACK (on_show_source), p);
  bar_button (row, _("Go to Text"), _("The paragraph that line was written from"),
              G_CALLBACK (on_go_to_text), p);
  gtk_box_append (GTK_BOX (p->error_bar), row);
  gtk_widget_set_visible (p->error_bar, FALSE);
  gtk_box_append (GTK_BOX (p->root), p->error_bar);

  p->stack = gtk_stack_new ();
  gtk_widget_set_vexpand (p->stack, TRUE);

  p->pdf = w42_pdf_view_new ();
  g_signal_connect (p->pdf, "point-activated", G_CALLBACK (on_point_activated), p);
  g_signal_connect (p->pdf, "scale-changed", G_CALLBACK (on_scale_changed), p);
  scroller = gtk_scrolled_window_new ();
  gtk_scrolled_window_set_child (GTK_SCROLLED_WINDOW (scroller), p->pdf);
  gtk_stack_add_named (GTK_STACK (p->stack), scroller, "pages");

  p->source = gtk_text_view_new ();
  gtk_text_view_set_editable (GTK_TEXT_VIEW (p->source), FALSE);
  gtk_text_view_set_monospace (GTK_TEXT_VIEW (p->source), TRUE);
  gtk_text_view_set_left_margin (GTK_TEXT_VIEW (p->source), 6);
  gtk_widget_add_css_class (p->source, "w42-latex-source");
  buffer = gtk_text_view_get_buffer (GTK_TEXT_VIEW (p->source));
  gtk_text_buffer_create_tag (buffer, "error", "paragraph-background", "#ffd0d0", NULL);
  click = gtk_gesture_click_new ();
  g_signal_connect (click, "pressed", G_CALLBACK (on_source_pressed), p);
  gtk_widget_add_controller (p->source, GTK_EVENT_CONTROLLER (click));
  scroller = gtk_scrolled_window_new ();
  gtk_scrolled_window_set_child (GTK_SCROLLED_WINDOW (scroller), p->source);
  gtk_stack_add_named (GTK_STACK (p->stack), scroller, "source");

  p->none = gtk_label_new ("");
  gtk_label_set_wrap (GTK_LABEL (p->none), TRUE);
  gtk_label_set_max_width_chars (GTK_LABEL (p->none), 40);
  gtk_label_set_justify (GTK_LABEL (p->none), GTK_JUSTIFY_CENTER);
  gtk_widget_set_margin_start (p->none, 18);
  gtk_widget_set_margin_end (p->none, 18);
  gtk_stack_add_named (GTK_STACK (p->stack), p->none, "none");
  gtk_label_set_text (GTK_LABEL (p->none), _("The document is being set with LaTeX..."));
  gtk_stack_set_visible_child_name (GTK_STACK (p->stack), "none");
  gtk_box_append (GTK_BOX (p->root), p->stack);

  /* The foot: what the engine is doing, and what it did. */
  foot = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 6);
  gtk_widget_add_css_class (foot, "w42-statusbar");
  p->spinner = gtk_spinner_new ();
  gtk_widget_set_visible (p->spinner, FALSE);
  gtk_box_append (GTK_BOX (foot), p->spinner);
  p->status = gtk_label_new ("");
  gtk_label_set_xalign (GTK_LABEL (p->status), 0.0);
  gtk_label_set_ellipsize (GTK_LABEL (p->status), PANGO_ELLIPSIZE_END);
  gtk_widget_set_hexpand (p->status, TRUE);
  gtk_box_append (GTK_BOX (foot), p->status);
  gtk_box_append (GTK_BOX (p->root), foot);

  g_signal_connect (p->root, "notify::visible", G_CALLBACK (on_visible), p);
  g_signal_connect (p->root, "destroy", G_CALLBACK (on_destroy), p);
  w42_latex_preview_set_view (p->root, view);
  return p->root;
}

void
w42_latex_preview_set_view (GtkWidget *preview, W42View *view)
{
  Preview *p = preview_of (preview);

  g_return_if_fail (p != NULL);
  g_return_if_fail (view == NULL || W42_IS_VIEW (view));

  if (p->view == view)
    return;
  if (p->view != NULL)
    {
      g_signal_handlers_disconnect_by_func (p->view, G_CALLBACK (on_view_state_changed), preview);
      g_object_remove_weak_pointer (G_OBJECT (p->view), (gpointer *) &p->view);
    }
  p->view = view;
  if (view != NULL)
    {
      g_object_add_weak_pointer (G_OBJECT (view), (gpointer *) &p->view);
      g_signal_connect_object (view, "state-changed", G_CALLBACK (on_view_state_changed), preview, 0);
      preview_watch_document (p, w42_view_get_document (view));
    }
}
