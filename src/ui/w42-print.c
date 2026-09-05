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

#include <string.h>

#include "w42-dialogs.h"

#include "w42-layout.h"
#include "w42-pdf.h"
#include "w42-preview.h"
#include "w42-settings.h"

/* The layout works in pixels at a fixed 96 dpi; a print context works in
 * points.  One scale factor reconciles them. */
#define PX_TO_POINTS (72.0 / W42_LAYOUT_DPI)

/* Word XP's Print > Options, the ones that mean something here, kept in
 * the print settings under these keys. */
#define KEY_REVERSE    "word42-reverse-order"
#define KEY_DRAWINGS   "word42-drawing-objects"
#define KEY_BACKGROUND "word42-background"
#define KEY_DRAFT      "word42-draft"

typedef struct {
  W42Document   *doc;
  W42PieceTable *selection;      /* printed instead of the document, if any */
  W42Layout     *layout;
  GtkWindow     *parent;
  gboolean       drawings, background, draft;
  char          *export_path;    /* Print to file: the PDF, and no printer */
  int            n_pages;

  /* The options tab's widgets, while the dialog is up. */
  GtkWidget     *w_reverse, *w_drawings, *w_background, *w_draft;
} PrintJob;

static GtkPrintSettings *saved_settings = NULL;

/* Where the settings live between sessions. */
static char *
settings_path (void)
{
  return g_build_filename (g_get_user_config_dir (), "word42", "print-settings.ini", NULL);
}

GtkPrintSettings *
w42_print_settings (void)
{
  if (saved_settings == NULL)
    {
      char *path = settings_path ();

      saved_settings = gtk_print_settings_new_from_file (path, NULL);
      if (saved_settings == NULL)
        saved_settings = gtk_print_settings_new ();
      g_free (path);
    }
  return saved_settings;
}

static void
remember_settings (GtkPrintSettings *settings)
{
  char *path = settings_path ();
  char *dir = g_path_get_dirname (path);

  if (saved_settings != settings)
    {
      g_clear_object (&saved_settings);
      saved_settings = g_object_ref (settings);
    }
  g_mkdir_with_parents (dir, 0700);
  gtk_print_settings_to_file (settings, path, NULL);
  g_free (dir);
  g_free (path);
}

static void
on_begin_print (GtkPrintOperation *operation,
                GtkPrintContext   *context,
                gpointer           data)
{
  PrintJob *job = data;
  GtkPrintSettings *settings = gtk_print_operation_get_print_settings (operation);

  (void) context;

  job->drawings = !gtk_print_settings_has_key (settings, KEY_DRAWINGS) ||
                  gtk_print_settings_get_bool (settings, KEY_DRAWINGS);
  job->background = gtk_print_settings_get_bool (settings, KEY_BACKGROUND);
  job->draft = gtk_print_settings_get_bool (settings, KEY_DRAFT);

  /* A layout of its own, always paginated, so that printing from Normal view
   * still produces pages; of the selection alone when that was asked for. */
  job->layout = w42_layout_new ();
  w42_layout_set_galley (job->layout, FALSE);
  if (job->selection != NULL &&
      gtk_print_settings_get_print_pages (settings) == GTK_PRINT_PAGES_SELECTION)
    {
      w42_layout_build_pt (job->layout, job->selection, w42_document_page_setup (job->doc));
      /* GTK prints "the selection" as page 0 of what it is given; the
       * pages of the fragment are all of it. */
      gtk_print_settings_set_print_pages (settings, GTK_PRINT_PAGES_ALL);
    }
  else
    w42_layout_build (job->layout, job->doc);

  job->n_pages = MAX (w42_layout_n_pages (job->layout), 1);
  gtk_print_operation_set_n_pages (operation, job->n_pages);
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
  int page = page_nr;

  (void) operation;

  cairo_save (cr);
  cairo_scale (cr, PX_TO_POINTS, PX_TO_POINTS);

  /* Format > Background, when asked for: Word left it off the paper
   * unless told otherwise, and so does this. */
  if (job->background)
    {
      const W42PageSetup *setup = w42_document_page_setup (job->doc);

      if (setup != NULL && setup->has_background)
        {
          cairo_set_source_rgb (cr, ((setup->background >> 16) & 0xFF) / 255.0,
                                ((setup->background >> 8) & 0xFF) / 255.0,
                                (setup->background & 0xFF) / 255.0);
          cairo_rectangle (cr, 0, 0, w42_layout_page_width (job->layout),
                           w42_layout_page_height (job->layout));
          cairo_fill (cr);
        }
    }
  cairo_set_source_rgb (cr, 0.0, 0.0, 0.0);

  /* Draft output: the text alone, as Word's draft printed -- no rules,
   * no shading, no pictures. */
  if (!job->draft)
    w42_layout_draw_backdrop (job->layout, cr, page);

  for (guint i = 0; i < lines->len; i++)
    {
      const W42LineBox *box = &g_array_index (lines, W42LineBox, i);

      if (box->page != page)
        continue;

      w42_layout_draw_line (job->layout, cr, box);
    }

  if (!job->draft)
    {
      if (job->drawings)
        w42_layout_draw_furniture (job->layout, cr, page);
      else
        {
          /* Without the drawing objects: the pictures and shapes set
           * beside the text are painted with the furniture, so they are
           * clipped away by painting nothing where they lie. */
          const GArray *floats = w42_layout_floats (job->layout);

          cairo_save (cr);
          cairo_rectangle (cr, 0, 0, w42_layout_page_width (job->layout), w42_layout_page_height (job->layout));
          for (guint i = 0; i < floats->len; i++)
            {
              const W42FloatBox *f = &g_array_index (floats, W42FloatBox, i);

              if (f->page == page)
                cairo_rectangle (cr, f->x + f->w, f->y, -f->w, f->h);
            }
          cairo_clip (cr);
          w42_layout_draw_furniture (job->layout, cr, page);
          cairo_restore (cr);
        }
    }

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

/* ---- The Word42 tab of the print dialog -------------------------------- */

static GObject *
on_create_custom_widget (GtkPrintOperation *operation, gpointer data)
{
  PrintJob *job = data;
  GtkPrintSettings *settings = gtk_print_operation_get_print_settings (operation);
  GtkWidget *box = gtk_box_new (GTK_ORIENTATION_VERTICAL, 6);

  gtk_widget_set_margin_top (box, 12);
  gtk_widget_set_margin_bottom (box, 12);
  gtk_widget_set_margin_start (box, 12);
  gtk_widget_set_margin_end (box, 12);

  job->w_reverse = gtk_check_button_new_with_mnemonic ("_Reverse print order");
  gtk_check_button_set_active (GTK_CHECK_BUTTON (job->w_reverse), gtk_print_settings_get_reverse (settings));
  job->w_drawings = gtk_check_button_new_with_mnemonic ("Print _drawing objects and wrapped pictures");
  job->w_background = gtk_check_button_new_with_mnemonic ("Print _background colour");
  job->w_draft = gtk_check_button_new_with_mnemonic ("Draft _output: the text alone");
  gtk_check_button_set_active (GTK_CHECK_BUTTON (job->w_reverse),
                               gtk_print_settings_get_bool (settings, KEY_REVERSE));
  gtk_check_button_set_active (GTK_CHECK_BUTTON (job->w_drawings),
                               !gtk_print_settings_has_key (settings, KEY_DRAWINGS) ||
                               gtk_print_settings_get_bool (settings, KEY_DRAWINGS));
  gtk_check_button_set_active (GTK_CHECK_BUTTON (job->w_background),
                               gtk_print_settings_get_bool (settings, KEY_BACKGROUND));
  gtk_check_button_set_active (GTK_CHECK_BUTTON (job->w_draft),
                               gtk_print_settings_get_bool (settings, KEY_DRAFT));
  gtk_box_append (GTK_BOX (box), job->w_reverse);
  gtk_box_append (GTK_BOX (box), job->w_drawings);
  gtk_box_append (GTK_BOX (box), job->w_background);
  gtk_box_append (GTK_BOX (box), job->w_draft);
  return G_OBJECT (box);
}

static void
on_custom_widget_apply (GtkPrintOperation *operation, GtkWidget *widget, gpointer data)
{
  PrintJob *job = data;
  GtkPrintSettings *settings = gtk_print_operation_get_print_settings (operation);

  (void) widget;
  gtk_print_settings_set_reverse (settings, gtk_check_button_get_active (GTK_CHECK_BUTTON (job->w_reverse)));
  gtk_print_settings_set_bool (settings, KEY_DRAWINGS,
                               gtk_check_button_get_active (GTK_CHECK_BUTTON (job->w_drawings)));
  gtk_print_settings_set_bool (settings, KEY_BACKGROUND,
                               gtk_check_button_get_active (GTK_CHECK_BUTTON (job->w_background)));
  gtk_print_settings_set_bool (settings, KEY_DRAFT,
                               gtk_check_button_get_active (GTK_CHECK_BUTTON (job->w_draft)));
}

static void offer_pdf_fallback (GtkWindow *parent, W42Document *doc, const char *why);

static void
on_done (GtkPrintOperation       *operation,
         GtkPrintOperationResult  result,
         gpointer                 data)
{
  PrintJob *job = data;

  g_printerr ("w42: print done result %d\n", (int) result);
  /* What the dialog was left at is what the next one opens with. */
  if (result == GTK_PRINT_OPERATION_RESULT_APPLY && job->export_path == NULL)
    remember_settings (gtk_print_operation_get_print_settings (operation));
  else if (result == GTK_PRINT_OPERATION_RESULT_ERROR)
    {
      GError *error = NULL;

      gtk_print_operation_get_error (operation, &error);
      offer_pdf_fallback (job->parent, job->doc,
                          error != NULL ? error->message : "No print backend is available.");
      g_clear_error (&error);
    }

  g_clear_pointer (&job->layout, w42_layout_free);
  g_clear_pointer (&job->selection, w42_pt_free);
  g_clear_object (&job->doc);
  g_free (job->export_path);
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

/* Runs the job: straight to the printer the settings name (or the
 * default), or through the system's dialog first. */
static void
run_print (GtkWindow *parent, W42Document *doc, W42PieceTable *selection,
           int current_page, GtkPrintSettings *settings, gboolean system_dialog,
           const char *export_path)
{
  GtkPrintOperation *operation;
  GtkPrintOperationResult result;
  GtkPageSetup *setup;
  GtkPaperSize *paper;
  const W42PageSetup *page;
  PrintJob *job;
  GError *error = NULL;
  const W42PrintExtras *extras = NULL;

  job = g_new0 (PrintJob, 1);
  job->doc = g_object_ref (doc);
  job->parent = parent;
  job->selection = selection;
  job->export_path = g_strdup (export_path);

  operation = gtk_print_operation_new ();
  gtk_print_operation_set_print_settings (operation, settings);
  if (export_path != NULL)
    gtk_print_operation_set_export_filename (operation, export_path);
  (void) extras;

  /* Word XP's "Selection" and "Current page", for the system dialogs
   * that show them. */
  gtk_print_operation_set_support_selection (operation, TRUE);
  gtk_print_operation_set_has_selection (operation, job->selection != NULL);
  if (current_page > 0)
    gtk_print_operation_set_current_page (operation, current_page - 1);

  /* Not run in the background: GTK's Windows backend shows no dialog at
   * all when asked to, and the pages are drawn in a moment anyway. */
  gtk_print_operation_set_custom_tab_label (operation, "Word42");

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
  g_signal_connect (operation, "create-custom-widget", G_CALLBACK (on_create_custom_widget), job);
  g_signal_connect (operation, "custom-widget-apply", G_CALLBACK (on_custom_widget_apply), job);
  g_signal_connect (operation, "done", G_CALLBACK (on_done), job);

  g_printerr ("w42: print run starting\n");
  result = gtk_print_operation_run (operation,
                                    export_path != NULL ? GTK_PRINT_OPERATION_ACTION_EXPORT
                                    : system_dialog ? GTK_PRINT_OPERATION_ACTION_PRINT_DIALOG
                                                    : GTK_PRINT_OPERATION_ACTION_PRINT,
                                    parent, &error);
  g_printerr ("w42: print run result %d error %s\n", (int) result, error != NULL ? error->message : "none");

  /* on_done has already reported an error with the operation's own
   * message; one that stopped the run before it began is reported here. */
  if (result == GTK_PRINT_OPERATION_RESULT_ERROR && error != NULL)
    g_clear_error (&error);

  gtk_paper_size_free (paper);
  g_object_unref (setup);
}

/* ---- Word XP's Print dialog -------------------------------------------- */

typedef struct {
  GtkWindow     *window;
  GtkWindow     *parent;
  W42Document   *doc;
  W42PieceTable *selection;
  int            current_page;
  GtkWidget     *range_all, *range_current, *range_selection, *range_pages, *pages;
  GtkWidget     *copies, *collate, *odd_even;
  GtkWidget     *reverse, *drawings, *background, *draft;
  GtkWidget     *printer;
} PrintBox;

static void
print_box_free (gpointer data)
{
  PrintBox *box = data;

  g_clear_pointer (&box->selection, w42_pt_free);
  g_clear_object (&box->doc);
  g_free (box);
}

/* "1,3,5-12" as GTK wants it; FALSE for nonsense. */
static gboolean
parse_page_ranges (const char *text, GtkPageRange **out, int *n_out)
{
  char **parts = g_strsplit (text, ",", -1);
  GArray *ranges = g_array_new (FALSE, FALSE, sizeof (GtkPageRange));
  gboolean ok = TRUE;

  for (int i = 0; parts[i] != NULL; i++)
    {
      char *part = g_strstrip (parts[i]);
      char *dash = strchr (part, '-');
      GtkPageRange r;

      if (*part == '\0')
        continue;
      if (dash != NULL)
        {
          *dash = '\0';
          r.start = atoi (part) - 1;
          r.end = atoi (dash + 1) - 1;
        }
      else
        r.start = r.end = atoi (part) - 1;
      if (r.start < 0 || r.end < r.start)
        {
          ok = FALSE;
          break;
        }
      g_array_append_val (ranges, r);
    }
  g_strfreev (parts);
  if (!ok || ranges->len == 0)
    {
      g_array_free (ranges, TRUE);
      return FALSE;
    }
  *n_out = (int) ranges->len;
  *out = (GtkPageRange *) g_array_free (ranges, FALSE);
  return TRUE;
}

/* What the box says, into the settings; FALSE with a message when the
 * page range makes no sense. */
static gboolean
print_box_apply (PrintBox *box, GtkPrintSettings *settings)
{
  guint oe = gtk_drop_down_get_selected (GTK_DROP_DOWN (box->odd_even));

  if (gtk_check_button_get_active (GTK_CHECK_BUTTON (box->range_current)) && box->current_page > 0)
    {
      GtkPageRange r = { box->current_page - 1, box->current_page - 1 };

      gtk_print_settings_set_print_pages (settings, GTK_PRINT_PAGES_RANGES);
      gtk_print_settings_set_page_ranges (settings, &r, 1);
    }
  else if (gtk_check_button_get_active (GTK_CHECK_BUTTON (box->range_selection)) && box->selection != NULL)
    gtk_print_settings_set_print_pages (settings, GTK_PRINT_PAGES_SELECTION);
  else if (gtk_check_button_get_active (GTK_CHECK_BUTTON (box->range_pages)))
    {
      GtkPageRange *ranges = NULL;
      int n = 0;

      if (!parse_page_ranges (gtk_editable_get_text (GTK_EDITABLE (box->pages)), &ranges, &n))
        {
          w42_message_show (box->window, "Type the pages to print the way Word did: 1,3,5-12.", NULL);
          return FALSE;
        }
      gtk_print_settings_set_print_pages (settings, GTK_PRINT_PAGES_RANGES);
      gtk_print_settings_set_page_ranges (settings, ranges, n);
      g_free (ranges);
    }
  else
    gtk_print_settings_set_print_pages (settings, GTK_PRINT_PAGES_ALL);

  gtk_print_settings_set_n_copies (settings, gtk_spin_button_get_value_as_int (GTK_SPIN_BUTTON (box->copies)));
  gtk_print_settings_set_collate (settings, gtk_check_button_get_active (GTK_CHECK_BUTTON (box->collate)));
  gtk_print_settings_set_page_set (settings, oe == 1 ? GTK_PAGE_SET_ODD : oe == 2 ? GTK_PAGE_SET_EVEN : GTK_PAGE_SET_ALL);
  gtk_print_settings_set_reverse (settings, gtk_check_button_get_active (GTK_CHECK_BUTTON (box->reverse)));
  gtk_print_settings_set_bool (settings, KEY_DRAWINGS, gtk_check_button_get_active (GTK_CHECK_BUTTON (box->drawings)));
  gtk_print_settings_set_bool (settings, KEY_BACKGROUND, gtk_check_button_get_active (GTK_CHECK_BUTTON (box->background)));
  gtk_print_settings_set_bool (settings, KEY_DRAFT, gtk_check_button_get_active (GTK_CHECK_BUTTON (box->draft)));
  return TRUE;
}

static void
on_print_box_go (GtkButton *button, gpointer data)
{
  PrintBox *box = data;
  GtkPrintSettings *settings = w42_print_settings ();
  gboolean system_dialog = g_str_equal (gtk_button_get_label (button), "P_rinter...");
  W42PieceTable *selection;

  if (!print_box_apply (box, settings))
    return;
  remember_settings (settings);
  /* The job takes the fragment; the box must not free it too. */
  selection = box->selection;
  box->selection = NULL;
  run_print (box->parent, box->doc, selection, box->current_page, settings, system_dialog, NULL);
  gtk_window_destroy (box->window);
}

static void
on_print_to_file_chosen (GObject *source, GAsyncResult *result, gpointer data)
{
  PrintBox *box = data;
  GFile *file = gtk_file_dialog_save_finish (GTK_FILE_DIALOG (source), result, NULL);
  GtkPrintSettings *settings = w42_print_settings ();
  W42PieceTable *selection;

  if (file == NULL)
    return;
  if (print_box_apply (box, settings))
    {
      char *path = g_file_get_path (file);

      remember_settings (settings);
      selection = box->selection;
      box->selection = NULL;
      run_print (box->parent, box->doc, selection, box->current_page, settings, FALSE, path);
      g_free (path);
      gtk_window_destroy (box->window);
    }
  g_object_unref (file);
}

/* Print to file: Word XP wrote the printer's language to a file; word42
 * writes the pages as a PDF, which any printer can be given, with the
 * range, the copies and the options applied. */
static void
on_print_to_file (GtkButton *button, gpointer data)
{
  PrintBox *box = data;
  GtkFileDialog *dialog = gtk_file_dialog_new ();
  GListStore *filters = g_list_store_new (GTK_TYPE_FILE_FILTER);
  GtkFileFilter *pdf = gtk_file_filter_new ();
  char *name = w42_document_get_title (box->doc);
  char *dot = strrchr (name, '.');
  char *suggested;

  (void) button;
  /* The document's name, with the PDF's extension for its own. */
  if (dot != NULL && dot != name && strlen (dot) <= 5)
    *dot = '\0';
  suggested = g_strconcat (name, ".pdf", NULL);
  gtk_file_filter_set_name (pdf, "PDF Documents (*.pdf)");
  gtk_file_filter_add_pattern (pdf, "*.pdf");
  g_list_store_append (filters, pdf);
  gtk_file_dialog_set_title (dialog, "Print to File");
  gtk_file_dialog_set_filters (dialog, G_LIST_MODEL (filters));
  gtk_file_dialog_set_initial_name (dialog, suggested);
  gtk_file_dialog_save (dialog, box->window, NULL, on_print_to_file_chosen, box);
  g_free (suggested);
  g_free (name);
  g_object_unref (filters);
  g_object_unref (dialog);
}

static void
on_print_box_cancel (GtkButton *button, gpointer data)
{
  PrintBox *box = data;

  (void) button;
  gtk_window_destroy (box->window);
}

static void
on_pages_typed (GtkEditable *entry, gpointer data)
{
  PrintBox *box = data;

  (void) entry;
  gtk_check_button_set_active (GTK_CHECK_BUTTON (box->range_pages), TRUE);
}

static GtkWidget *
frame_with_grid (GtkWidget *parent_box, const char *title, GtkWidget **grid_out)
{
  GtkWidget *frame = gtk_frame_new (title);
  GtkWidget *grid = gtk_grid_new ();

  gtk_grid_set_row_spacing (GTK_GRID (grid), 4);
  gtk_grid_set_column_spacing (GTK_GRID (grid), 8);
  gtk_widget_set_margin_top (grid, 6);
  gtk_widget_set_margin_bottom (grid, 6);
  gtk_widget_set_margin_start (grid, 8);
  gtk_widget_set_margin_end (grid, 8);
  gtk_frame_set_child (GTK_FRAME (frame), grid);
  gtk_box_append (GTK_BOX (parent_box), frame);
  *grid_out = grid;
  return frame;
}

static void
print_dialog_show (GtkWindow *parent, W42Document *doc, const W42PrintExtras *extras)
{
  PrintBox *box = g_new0 (PrintBox, 1);
  GtkPrintSettings *settings = w42_print_settings ();
  GtkWidget *content, *grid, *columns, *left, *right, *buttons, *b;
  const char *printer = gtk_print_settings_get_printer (settings);
  char *title;
  static const char *const odd_even[] = { "All pages in range", "Odd pages", "Even pages", NULL };

  box->parent = parent;
  box->doc = g_object_ref (doc);
  box->selection = extras != NULL ? extras->selection : NULL;
  box->current_page = extras != NULL ? extras->current_page : 0;

  box->window = GTK_WINDOW (gtk_window_new ());
  gtk_window_set_title (box->window, "Print");
  gtk_window_set_transient_for (box->window, parent);
  gtk_window_set_modal (box->window, TRUE);
  gtk_window_set_resizable (box->window, FALSE);
  gtk_widget_add_css_class (GTK_WIDGET (box->window), "w42");
  gtk_widget_add_css_class (GTK_WIDGET (box->window), "w42-dialog");
  g_object_set_data_full (G_OBJECT (box->window), "w42-print-box", box, print_box_free);

  content = gtk_box_new (GTK_ORIENTATION_VERTICAL, 8);
  gtk_widget_set_margin_top (content, 10);
  gtk_widget_set_margin_bottom (content, 10);
  gtk_widget_set_margin_start (content, 10);
  gtk_widget_set_margin_end (content, 10);
  gtk_window_set_child (box->window, content);

  /* The printer: the one the last job went to, or the system's default. */
  frame_with_grid (content, "Printer", &grid);
  title = g_strdup_printf ("Name:  %s", printer != NULL && *printer != '\0' ? printer : "(the default printer)");
  box->printer = gtk_label_new (title);
  gtk_label_set_xalign (GTK_LABEL (box->printer), 0.0);
  gtk_grid_attach (GTK_GRID (grid), box->printer, 0, 0, 1, 1);
  g_free (title);

  columns = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 8);
  gtk_box_append (GTK_BOX (content), columns);
  left = gtk_box_new (GTK_ORIENTATION_VERTICAL, 8);
  right = gtk_box_new (GTK_ORIENTATION_VERTICAL, 8);
  gtk_widget_set_hexpand (left, TRUE);
  gtk_widget_set_hexpand (right, TRUE);
  gtk_box_append (GTK_BOX (columns), left);
  gtk_box_append (GTK_BOX (columns), right);

  /* Page range, as Word XP's dialog had it. */
  frame_with_grid (left, "Page range", &grid);
  box->range_all = gtk_check_button_new_with_mnemonic ("_All");
  box->range_current = gtk_check_button_new_with_mnemonic ("Curr_ent page");
  box->range_selection = gtk_check_button_new_with_mnemonic ("_Selection");
  box->range_pages = gtk_check_button_new_with_mnemonic ("Pa_ges:");
  gtk_check_button_set_group (GTK_CHECK_BUTTON (box->range_current), GTK_CHECK_BUTTON (box->range_all));
  gtk_check_button_set_group (GTK_CHECK_BUTTON (box->range_selection), GTK_CHECK_BUTTON (box->range_all));
  gtk_check_button_set_group (GTK_CHECK_BUTTON (box->range_pages), GTK_CHECK_BUTTON (box->range_all));
  gtk_check_button_set_active (GTK_CHECK_BUTTON (box->range_all), TRUE);
  gtk_widget_set_sensitive (box->range_current, box->current_page > 0);
  gtk_widget_set_sensitive (box->range_selection, box->selection != NULL);
  box->pages = gtk_entry_new ();
  gtk_entry_set_placeholder_text (GTK_ENTRY (box->pages), "1,3,5-12");
  gtk_editable_set_width_chars (GTK_EDITABLE (box->pages), 12);
  g_signal_connect (box->pages, "changed", G_CALLBACK (on_pages_typed), box);
  gtk_grid_attach (GTK_GRID (grid), box->range_all, 0, 0, 2, 1);
  gtk_grid_attach (GTK_GRID (grid), box->range_current, 0, 1, 1, 1);
  gtk_grid_attach (GTK_GRID (grid), box->range_selection, 1, 1, 1, 1);
  gtk_grid_attach (GTK_GRID (grid), box->range_pages, 0, 2, 1, 1);
  gtk_grid_attach (GTK_GRID (grid), box->pages, 1, 2, 1, 1);
  {
    GtkWidget *hint = gtk_label_new ("Enter page numbers and/or page ranges\nseparated by commas.  For example, 1,3,5-12");

    gtk_label_set_xalign (GTK_LABEL (hint), 0.0);
    gtk_widget_add_css_class (hint, "w42-dialog-status");
    gtk_grid_attach (GTK_GRID (grid), hint, 0, 3, 2, 1);
  }

  /* Copies. */
  frame_with_grid (right, "Copies", &grid);
  {
    GtkWidget *label = gtk_label_new_with_mnemonic ("Number of _copies:");

    box->copies = gtk_spin_button_new_with_range (1, 999, 1);
    gtk_spin_button_set_value (GTK_SPIN_BUTTON (box->copies), MAX (gtk_print_settings_get_n_copies (settings), 1));
    gtk_label_set_mnemonic_widget (GTK_LABEL (label), box->copies);
    gtk_label_set_xalign (GTK_LABEL (label), 0.0);
    gtk_grid_attach (GTK_GRID (grid), label, 0, 0, 1, 1);
    gtk_grid_attach (GTK_GRID (grid), box->copies, 1, 0, 1, 1);
    box->collate = gtk_check_button_new_with_mnemonic ("Colla_te");
    gtk_check_button_set_active (GTK_CHECK_BUTTON (box->collate),
                                 !gtk_print_settings_has_key (settings, GTK_PRINT_SETTINGS_COLLATE) ||
                                 gtk_print_settings_get_collate (settings));
    gtk_grid_attach (GTK_GRID (grid), box->collate, 0, 1, 2, 1);
  }

  /* Print: all, odd or even pages; and Word's Options, the ones that mean
   * something here. */
  frame_with_grid (left, "Print", &grid);
  box->odd_even = gtk_drop_down_new_from_strings (odd_even);
  gtk_drop_down_set_selected (GTK_DROP_DOWN (box->odd_even),
                              gtk_print_settings_get_page_set (settings) == GTK_PAGE_SET_ODD ? 1
                              : gtk_print_settings_get_page_set (settings) == GTK_PAGE_SET_EVEN ? 2 : 0);
  gtk_grid_attach (GTK_GRID (grid), box->odd_even, 0, 0, 1, 1);

  frame_with_grid (right, "Options", &grid);
  box->reverse = gtk_check_button_new_with_mnemonic ("_Reverse print order");
  box->drawings = gtk_check_button_new_with_mnemonic ("_Drawing objects");
  box->background = gtk_check_button_new_with_mnemonic ("_Background colour");
  box->draft = gtk_check_button_new_with_mnemonic ("Draft _output");
  gtk_check_button_set_active (GTK_CHECK_BUTTON (box->reverse), gtk_print_settings_get_reverse (settings));
  gtk_check_button_set_active (GTK_CHECK_BUTTON (box->drawings),
                               !gtk_print_settings_has_key (settings, KEY_DRAWINGS) ||
                               gtk_print_settings_get_bool (settings, KEY_DRAWINGS));
  gtk_check_button_set_active (GTK_CHECK_BUTTON (box->background), gtk_print_settings_get_bool (settings, KEY_BACKGROUND));
  gtk_check_button_set_active (GTK_CHECK_BUTTON (box->draft), gtk_print_settings_get_bool (settings, KEY_DRAFT));
  gtk_grid_attach (GTK_GRID (grid), box->reverse, 0, 0, 1, 1);
  gtk_grid_attach (GTK_GRID (grid), box->drawings, 0, 1, 1, 1);
  gtk_grid_attach (GTK_GRID (grid), box->background, 0, 2, 1, 1);
  gtk_grid_attach (GTK_GRID (grid), box->draft, 0, 3, 1, 1);

  /* Print goes to the printer named; Printer... opens the system's own
   * dialog to choose another, and prints from there. */
  buttons = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 8);
  gtk_widget_set_halign (buttons, GTK_ALIGN_END);
  b = gtk_button_new_with_mnemonic ("_Print");
  g_signal_connect (b, "clicked", G_CALLBACK (on_print_box_go), box);
  gtk_box_append (GTK_BOX (buttons), b);
  gtk_window_set_default_widget (box->window, b);
  b = gtk_button_new_with_mnemonic ("P_rinter...");
  g_signal_connect (b, "clicked", G_CALLBACK (on_print_box_go), box);
  gtk_box_append (GTK_BOX (buttons), b);
  b = gtk_button_new_with_mnemonic ("Print to _file...");
  g_signal_connect (b, "clicked", G_CALLBACK (on_print_to_file), box);
  gtk_box_append (GTK_BOX (buttons), b);
  b = gtk_button_new_with_mnemonic ("Cancel");
  g_signal_connect (b, "clicked", G_CALLBACK (on_print_box_cancel), box);
  gtk_box_append (GTK_BOX (buttons), b);
  gtk_box_append (GTK_BOX (content), buttons);

  gtk_window_present (box->window);
}

void
w42_print_document (GtkWindow *parent, W42Document *doc, gboolean preview,
                    const W42PrintExtras *extras)
{
  g_return_if_fail (W42_IS_DOCUMENT (doc));

  if (preview)
    {
      if (extras != NULL && extras->selection != NULL)
        w42_pt_free (extras->selection);
      gtk_window_present (GTK_WINDOW (w42_preview_new (parent, doc)));
      return;
    }
  print_dialog_show (parent, doc, extras);
}
