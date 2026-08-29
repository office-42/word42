/* w42-print.c - see w42-print.h
 *
 * Copyright (C) 2026 Andreas Røsdal
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * Printing uses the same layout engine the screen uses, in Page Layout mode.
 * That is the whole payoff for having done pagination properly rather than
 * scrolling one long galley: what comes out of the printer breaks where the
 * screen said it would, because it is the same pagination.
 *
 * The print dialog is GTK's, which on Windows is the system's own, on Linux
 * is CUPS, and on macOS is GTK's portable one printing through lpr.  Where a
 * platform has no working print backend at all, the fallback is to write the
 * same pages to a PDF -- which every platform can then print itself.
 */

#include "w42-print.h"

#include "w42-dialogs.h"

#include "w42-layout.h"
#include "w42-pdf.h"
#include "w42-preview.h"

/* The layout works in pixels at a fixed 96 dpi; a print context works in
 * points.  One scale factor reconciles them. */
#define PX_TO_POINTS (72.0 / W42_LAYOUT_DPI)

typedef struct {
  W42Document *doc;
  W42Layout   *layout;
} PrintJob;

static void
on_begin_print (GtkPrintOperation *operation,
                GtkPrintContext   *context,
                gpointer           data)
{
  PrintJob *job = data;

  (void) context;

  /* A layout of its own, always paginated, so that printing from Normal view
   * still produces pages. */
  job->layout = w42_layout_new ();
  w42_layout_set_galley (job->layout, FALSE);
  w42_layout_build (job->layout, job->doc);

  gtk_print_operation_set_n_pages (operation,
                                   w42_layout_n_pages (job->layout));
}

static void
on_draw_page (GtkPrintOperation *operation,
              GtkPrintContext   *context,
              int                page_nr,
              gpointer           data)
{
  PrintJob *job = data;
  cairo_t *cr = gtk_print_context_get_cairo_context (context);
  const GArray *lines = w42_layout_lines (job->layout);

  (void) operation;

  cairo_save (cr);
  cairo_scale (cr, PX_TO_POINTS, PX_TO_POINTS);
  cairo_set_source_rgb (cr, 0.0, 0.0, 0.0);

  w42_layout_draw_backdrop (job->layout, cr, page_nr);

  for (guint i = 0; i < lines->len; i++)
    {
      const W42LineBox *box = &g_array_index (lines, W42LineBox, i);

      if (box->page != page_nr)
        continue;

      w42_layout_draw_line (job->layout, cr, box);
    }

  w42_layout_draw_furniture (job->layout, cr, page_nr);

  cairo_restore (cr);
}

static void
on_end_print (GtkPrintOperation *operation,
              GtkPrintContext   *context,
              gpointer           data)
{
  PrintJob *job = data;

  (void) operation; (void) context;

  g_clear_pointer (&job->layout, w42_layout_free);
}

static void
on_done (GtkPrintOperation       *operation,
         GtkPrintOperationResult  result,
         gpointer                 data)
{
  PrintJob *job = data;

  (void) result;

  g_clear_pointer (&job->layout, w42_layout_free);
  g_clear_object (&job->doc);
  g_free (job);
  g_object_unref (operation);
}

/* ---- The PDF fallback -------------------------------------------------- */

static void
on_fallback_save (GObject *source, GAsyncResult *result, gpointer data)
{
  W42Document *doc = data;
  GError *error = NULL;
  GFile *file;

  file = gtk_file_dialog_save_finish (GTK_FILE_DIALOG (source), result, &error);

  if (file != NULL)
    {
      if (!w42_pdf_export (w42_document_pt (doc), w42_document_page_setup (doc),
                           file, &error))
        {
          w42_message_show (NULL, "Word42 could not write the PDF.", error->message);
        }
      g_object_unref (file);
    }

  g_clear_error (&error);
  g_object_unref (doc);
}

static void
on_fallback_choice (GObject *source, GAsyncResult *result, gpointer data)
{
  W42Document *doc = data;
  GtkWindow *parent = g_object_get_data (G_OBJECT (source), "parent");
  int choice;

  choice = gtk_alert_dialog_choose_finish (GTK_ALERT_DIALOG (source), result, NULL);

  if (choice == 1)
    {
      GtkFileDialog *dialog = gtk_file_dialog_new ();
      GListStore *filters = g_list_store_new (GTK_TYPE_FILE_FILTER);
      GtkFileFilter *pdf = gtk_file_filter_new ();
      char *name = w42_document_get_title (doc);
      char *suggested = g_strconcat (name, ".pdf", NULL);

      gtk_file_filter_set_name (pdf, "PDF Documents (*.pdf)");
      gtk_file_filter_add_pattern (pdf, "*.pdf");
      g_list_store_append (filters, pdf);

      gtk_file_dialog_set_title (dialog, "Print to PDF");
      gtk_file_dialog_set_filters (dialog, G_LIST_MODEL (filters));
      gtk_file_dialog_set_initial_name (dialog, suggested);
      gtk_file_dialog_save (dialog, parent, NULL, on_fallback_save, doc);

      g_free (suggested);
      g_free (name);
      g_object_unref (filters);
      g_object_unref (dialog);
      return;
    }

  g_object_unref (doc);
}

/* No print backend could take the job.  Offer the PDF, which is what the
 * job would have become on the way to most printers anyway. */
static void
offer_pdf_fallback (GtkWindow *parent, W42Document *doc, const char *why)
{
  GtkAlertDialog *dialog;
  static const char *buttons[] = { "Cancel", "Print to PDF...", NULL };
  char *detail;

  detail = g_strdup_printf ("%s\n\nword42 can write the document to a PDF "
                            "instead, which you can print from any PDF "
                            "viewer.", why);

  dialog = gtk_alert_dialog_new ("Word42 could not print the document.");
  gtk_alert_dialog_set_detail (dialog, detail);
  gtk_alert_dialog_set_buttons (dialog, buttons);
  gtk_alert_dialog_set_cancel_button (dialog, 0);
  gtk_alert_dialog_set_default_button (dialog, 1);
  g_object_set_data (G_OBJECT (dialog), "parent", parent);
  gtk_alert_dialog_choose (dialog, parent, NULL, on_fallback_choice,
                           g_object_ref (doc));

  g_free (detail);
  g_object_unref (dialog);
}

void
w42_print_document (GtkWindow *parent, W42Document *doc, gboolean preview)
{
  GtkPrintOperation *operation;
  GtkPrintOperationResult result;
  GtkPageSetup *setup;
  GtkPaperSize *paper;
  const W42PageSetup *page;
  PrintJob *job;
  GError *error = NULL;

  g_return_if_fail (W42_IS_DOCUMENT (doc));

  if (preview)
    {
      gtk_window_present (GTK_WINDOW (w42_preview_new (parent, doc)));
      return;
    }

  job = g_new0 (PrintJob, 1);
  job->doc = g_object_ref (doc);

  operation = gtk_print_operation_new ();

  /* Tell the printer the paper the document was laid out for, and ask for no
   * margins of its own: the layout has already put the margins in, and a
   * second set applied on top would inset the text twice. */
  page = w42_document_page_setup (doc);
  paper = gtk_paper_size_new_custom ("w42", "Word42 page",
                                     page->width / 20.0, page->height / 20.0,
                                     GTK_UNIT_POINTS);
  setup = gtk_page_setup_new ();
  gtk_page_setup_set_paper_size (setup, paper);
  gtk_page_setup_set_top_margin (setup, 0, GTK_UNIT_POINTS);
  gtk_page_setup_set_bottom_margin (setup, 0, GTK_UNIT_POINTS);
  gtk_page_setup_set_left_margin (setup, 0, GTK_UNIT_POINTS);
  gtk_page_setup_set_right_margin (setup, 0, GTK_UNIT_POINTS);
  gtk_page_setup_set_orientation (setup,
    page->width > page->height ? GTK_PAGE_ORIENTATION_LANDSCAPE
                               : GTK_PAGE_ORIENTATION_PORTRAIT);
  gtk_print_operation_set_default_page_setup (operation, setup);
  gtk_print_operation_set_use_full_page (operation, TRUE);
  gtk_print_operation_set_unit (operation, GTK_UNIT_POINTS);
  gtk_print_operation_set_embed_page_setup (operation, TRUE);

  {
    char *name = w42_document_get_title (doc);
    gtk_print_operation_set_job_name (operation, name);
    g_free (name);
  }

  g_signal_connect (operation, "begin-print", G_CALLBACK (on_begin_print), job);
  g_signal_connect (operation, "draw-page", G_CALLBACK (on_draw_page), job);
  g_signal_connect (operation, "end-print", G_CALLBACK (on_end_print), job);
  g_signal_connect (operation, "done", G_CALLBACK (on_done), job);

  result = gtk_print_operation_run (operation,
                                    GTK_PRINT_OPERATION_ACTION_PRINT_DIALOG,
                                    parent, &error);

  if (result == GTK_PRINT_OPERATION_RESULT_ERROR)
    {
      offer_pdf_fallback (parent, doc,
                          error != NULL ? error->message
                                        : "No print backend is available.");
      g_clear_error (&error);
    }

  gtk_paper_size_free (paper);
  g_object_unref (setup);
}
