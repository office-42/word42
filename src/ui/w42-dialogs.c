/* w42-dialogs.c - see w42-dialogs.h
 *
 * Copyright (C) 2026 Andreas Røsdal
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "w42-dialogs.h"

#include "w42-autocorrect.h"
#include "w42-autotext.h"
#include "w42-envelope.h"
#include "w42-template.h"
#include "w42-tableformat.h"
#include "w42-lang.h"
#include "w42-spell.h"

#include "w42-settings.h"
#include "w42-merge.h"
#include "w42-shape.h"
#include "w42-window.h"

#include <math.h>
#include <string.h>

/* Measurements are entered in the user's unit -- inches unless Tools >
 * Options says centimetres -- and stored in twips, as Word 6 stored them. */
#define TWIPS_PER_INCH 1440.0

static double
measure_from_twips (double twips)
{
  return w42_settings_from_twips ((int) twips);
}

static double
twips_from_measure (double value)
{
  return w42_settings_to_twips (value);
}

/* ---------------------------------------------------------------------- */
/* Shared furniture                                                        */
/* ---------------------------------------------------------------------- */

/* Escape closes a dialog.  GtkWindow does not do that of its own accord, and
 * a dialog that will not go away when you press Escape is the first thing a
 * person notices. */
static gboolean
on_dialog_key (GtkEventControllerKey *controller, guint keyval,
               guint keycode, GdkModifierType state, gpointer data)
{
  (void) controller; (void) keycode; (void) state;

  if (keyval == GDK_KEY_Escape)
    {
      gtk_window_close (GTK_WINDOW (data));
      return GDK_EVENT_STOP;
    }

  return GDK_EVENT_PROPAGATE;
}

/* When a dialog goes, the keyboard goes back to the document.  Left to
 * itself GTK hands focus to whichever widget is next in the window, and the
 * first Tab after closing a dialog then walks the toolbar instead of the
 * table cell it was meant for. */
static void
on_dialog_destroy (GtkWidget *dialog, gpointer view)
{
  (void) dialog;

  if (GTK_IS_WIDGET (view))
    gtk_widget_grab_focus (GTK_WIDGET (view));
}

static GtkWidget *
dialog_shell (GtkWindow *parent, const char *title, GtkWidget **content,
              W42View *view)
{
  GtkWidget *window = gtk_window_new ();
  GtkWidget *box = gtk_box_new (GTK_ORIENTATION_VERTICAL, 12);
  GtkEventController *key = gtk_event_controller_key_new ();

  g_signal_connect (key, "key-pressed", G_CALLBACK (on_dialog_key), window);
  gtk_widget_add_controller (window, key);
  /* Bound to the view's life: a box destroyed after its window's children
   * must not reach into a view that is gone. */
  g_signal_connect_object (window, "destroy", G_CALLBACK (on_dialog_destroy), view, 0);

  gtk_window_set_title (GTK_WINDOW (window), title);
  gtk_window_set_transient_for (GTK_WINDOW (window), parent);
  gtk_window_set_destroy_with_parent (GTK_WINDOW (window), TRUE);
  gtk_window_set_modal (GTK_WINDOW (window), TRUE);
  gtk_window_set_resizable (GTK_WINDOW (window), FALSE);

  /* A dialog is a window of its own, so it needs the chrome class the
   * main window carries for the period font and colours to reach it. */
  gtk_widget_add_css_class (window, "w42");
  gtk_widget_add_css_class (box, "w42-dialog");
  gtk_widget_set_margin_start (box, 14);
  gtk_widget_set_margin_end (box, 14);
  gtk_widget_set_margin_top (box, 14);
  gtk_widget_set_margin_bottom (box, 14);
  gtk_window_set_child (GTK_WINDOW (window), box);

  *content = box;
  return window;
}

/* A titled group of controls, the way every dialog of the period framed
 * one. */
static GtkWidget *
group (GtkWidget *parent, const char *title)
{
  GtkWidget *frame = gtk_frame_new (title);
  GtkWidget *grid = gtk_grid_new ();

  gtk_grid_set_row_spacing (GTK_GRID (grid), 6);
  gtk_grid_set_column_spacing (GTK_GRID (grid), 10);
  gtk_widget_set_margin_start (grid, 10);
  gtk_widget_set_margin_end (grid, 10);
  gtk_widget_set_margin_top (grid, 8);
  gtk_widget_set_margin_bottom (grid, 10);

  gtk_frame_set_child (GTK_FRAME (frame), grid);
  gtk_box_append (GTK_BOX (parent), frame);

  return grid;
}

static GtkWidget *
inches_row (GtkWidget *grid, int row, int col, const char *label, double value)
{
  GtkWidget *text = gtk_label_new_with_mnemonic (label);
  gboolean cm = w42_settings_get_units () == W42_UNITS_CM;
  GtkWidget *spin = gtk_spin_button_new_with_range (0.0, cm ? 56.0 : 22.0,
                                                    cm ? 0.25 : 0.1);

  gtk_label_set_xalign (GTK_LABEL (text), 0.0);
  gtk_label_set_mnemonic_widget (GTK_LABEL (text), spin);

  gtk_spin_button_set_digits (GTK_SPIN_BUTTON (spin), 2);
  gtk_spin_button_set_value (GTK_SPIN_BUTTON (spin), value);
  gtk_widget_set_size_request (spin, 84, -1);

  /* Enter in a spinner presses OK, as Enter in a text field does.  GTK
   * leaves that off by default, and a dialog that ignores Enter feels
   * broken. */
  gtk_spin_button_set_activates_default (GTK_SPIN_BUTTON (spin), TRUE);

  gtk_grid_attach (GTK_GRID (grid), text, col * 2, row, 1, 1);
  gtk_grid_attach (GTK_GRID (grid), spin, col * 2 + 1, row, 1, 1);

  return spin;
}

static GtkWidget *
choice_row (GtkWidget *grid, int row, int col, const char *label,
            const char * const *options, guint selected)
{
  GtkWidget *text = gtk_label_new_with_mnemonic (label);
  GtkWidget *drop = options != NULL
                      ? gtk_drop_down_new_from_strings (options)
                      : gtk_drop_down_new (NULL, NULL);

  gtk_label_set_xalign (GTK_LABEL (text), 0.0);
  gtk_label_set_mnemonic_widget (GTK_LABEL (text), drop);
  gtk_drop_down_set_selected (GTK_DROP_DOWN (drop), selected);
  gtk_widget_set_size_request (drop, 132, -1);

  gtk_grid_attach (GTK_GRID (grid), text, col * 2, row, 1, 1);
  gtk_grid_attach (GTK_GRID (grid), drop, col * 2 + 1, row, 1, 1);

  return drop;
}

static GtkWidget *
button_row (GtkWidget *parent, GtkWidget *window,
            GCallback on_ok, gpointer data)
{
  GtkWidget *row = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 8);
  GtkWidget *ok = gtk_button_new_with_mnemonic ("_OK");
  GtkWidget *cancel = gtk_button_new_with_mnemonic ("Cancel");

  gtk_widget_set_halign (row, GTK_ALIGN_END);
  gtk_widget_set_size_request (ok, 92, 26);
  gtk_widget_set_size_request (cancel, 92, 26);

  g_signal_connect (ok, "clicked", on_ok, data);
  g_signal_connect_swapped (cancel, "clicked",
                            G_CALLBACK (gtk_window_destroy), window);

  gtk_box_append (GTK_BOX (row), ok);
  gtk_box_append (GTK_BOX (row), cancel);
  gtk_box_append (GTK_BOX (parent), row);

  gtk_window_set_default_widget (GTK_WINDOW (window), ok);
  return ok;
}

/* ---------------------------------------------------------------------- */
/* A message box                                                           */
/* ---------------------------------------------------------------------- */

/* The system's alert dialog is styled and worded by the desktop -- its
 * button says "Close" in the desktop's language, next to a program that
 * is in English and drawn as 1993 -- so Word42 puts up its own, with the
 * chrome the rest of the program wears and one OK button. */
void
w42_message_show (GtkWindow *parent, const char *heading, const char *detail)
{
  GtkWidget *window, *content, *label, *row, *ok;

  g_return_if_fail (heading != NULL);

  window = dialog_shell (parent, "Word42", &content, NULL);

  label = gtk_label_new (heading);
  gtk_label_set_wrap (GTK_LABEL (label), TRUE);
  gtk_label_set_max_width_chars (GTK_LABEL (label), 52);
  gtk_label_set_xalign (GTK_LABEL (label), 0.0);
  gtk_box_append (GTK_BOX (content), label);

  if (detail != NULL && *detail != '\0')
    {
      GtkWidget *more = gtk_label_new (detail);

      gtk_label_set_wrap (GTK_LABEL (more), TRUE);
      gtk_label_set_max_width_chars (GTK_LABEL (more), 52);
      gtk_label_set_xalign (GTK_LABEL (more), 0.0);
      gtk_widget_add_css_class (more, "w42-dialog-status");
      gtk_box_append (GTK_BOX (content), more);
    }

  row = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 8);
  ok = gtk_button_new_with_mnemonic ("_OK");
  gtk_widget_set_halign (row, GTK_ALIGN_END);
  gtk_widget_set_size_request (ok, 92, 26);
  g_signal_connect_swapped (ok, "clicked", G_CALLBACK (gtk_window_destroy), window);
  gtk_box_append (GTK_BOX (row), ok);
  gtk_box_append (GTK_BOX (content), row);
  gtk_window_set_default_widget (GTK_WINDOW (window), ok);

  gtk_window_present (GTK_WINDOW (window));
  gtk_widget_grab_focus (ok);
}

/* The same, with a row of buttons: the answer comes back as the index of
 * the button pressed, and dismissing the box -- Escape, or its close
 * button -- answers with `cancel`, since a stray keypress must never be
 * taken for "yes, throw the document away". */
typedef struct {
  W42ChoiceFunc func;
  gpointer      data;
  int           answer;
  int           cancel;
  gboolean      answered;
} ChoiceBox;

static void
choice_free (gpointer data, GObject *gone)
{
  ChoiceBox *box = data;

  (void) gone;
  if (box->func != NULL)
    box->func (box->answered ? box->answer : box->cancel, box->data);
  g_free (box);
}

static void
on_choice_clicked (GtkButton *button, gpointer data)
{
  ChoiceBox *box = data;

  box->answer = GPOINTER_TO_INT (g_object_get_data (G_OBJECT (button), "w42-choice"));
  box->answered = TRUE;
  gtk_window_destroy (GTK_WINDOW (gtk_widget_get_root (GTK_WIDGET (button))));
}

void
w42_choice_show (GtkWindow          *parent,
                 const char         *heading,
                 const char         *detail,
                 const char * const *labels,
                 int                 default_button,
                 int                 cancel_button,
                 W42ChoiceFunc       func,
                 gpointer            data)
{
  GtkWidget *window, *content, *label, *row;
  ChoiceBox *box;

  g_return_if_fail (heading != NULL);
  g_return_if_fail (labels != NULL && labels[0] != NULL);

  box = g_new0 (ChoiceBox, 1);
  box->func = func;
  box->data = data;
  box->cancel = cancel_button;

  window = dialog_shell (parent, "Word42", &content, NULL);
  g_object_weak_ref (G_OBJECT (window), choice_free, box);

  label = gtk_label_new (heading);
  gtk_label_set_wrap (GTK_LABEL (label), TRUE);
  gtk_label_set_max_width_chars (GTK_LABEL (label), 52);
  gtk_label_set_xalign (GTK_LABEL (label), 0.0);
  gtk_box_append (GTK_BOX (content), label);

  if (detail != NULL && *detail != '\0')
    {
      GtkWidget *more = gtk_label_new (detail);

      gtk_label_set_wrap (GTK_LABEL (more), TRUE);
      gtk_label_set_max_width_chars (GTK_LABEL (more), 52);
      gtk_label_set_xalign (GTK_LABEL (more), 0.0);
      gtk_widget_add_css_class (more, "w42-dialog-status");
      gtk_box_append (GTK_BOX (content), more);
    }

  row = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 8);
  gtk_widget_set_halign (row, GTK_ALIGN_END);
  for (int i = 0; labels[i] != NULL; i++)
    {
      GtkWidget *button = gtk_button_new_with_mnemonic (labels[i]);

      gtk_widget_set_size_request (button, 92, 26);
      g_object_set_data (G_OBJECT (button), "w42-choice", GINT_TO_POINTER (i));
      g_signal_connect (button, "clicked", G_CALLBACK (on_choice_clicked), box);
      gtk_box_append (GTK_BOX (row), button);
      if (i == default_button)
        {
          gtk_window_set_default_widget (GTK_WINDOW (window), button);
          gtk_widget_grab_focus (button);
        }
    }
  gtk_box_append (GTK_BOX (content), row);

  gtk_window_present (GTK_WINDOW (window));
}

/* ---------------------------------------------------------------------- */
/* Page Setup                                                              */
/* ---------------------------------------------------------------------- */

typedef struct {
  GtkWidget   *window;
  W42View     *view;
  GtkWidget   *top, *bottom, *left, *right;
  GtkWidget   *paper, *orientation;
} PageSetupBox;

/* Width and height in twips, portrait. */
static const int PAPER_SIZES[][2] = {
  { 12240, 15840 },   /* Letter    8.5 x 11    */
  { 12240, 20160 },   /* Legal     8.5 x 14    */
  { 11906, 16838 },   /* A4      210 x 297 mm  */
  {  8391, 11906 },   /* A5      148 x 210 mm  */
};

static const char * const PAPER_NAMES[] = {
  "Letter  8\302\275 x 11 in",
  "Legal  8\302\275 x 14 in",
  "A4  210 x 297 mm",
  "A5  148 x 210 mm",
  "Custom (as it is)", NULL
};

static const char * const ORIENTATIONS[] = { "Portrait", "Landscape", NULL };

static void
page_setup_free (gpointer data, GObject *gone)
{
  (void) gone;
  g_free (data);
}

static void
on_page_setup_ok (GtkButton *button, gpointer data)
{
  PageSetupBox *box = data;
  W42Document *doc = w42_view_get_document (box->view);
  W42PageSetup page;
  guint paper, landscape;

  (void) button;

  if (doc == NULL)
    return;

  page = *w42_document_page_setup (doc);

  paper = gtk_drop_down_get_selected (GTK_DROP_DOWN (box->paper));
  landscape = gtk_drop_down_get_selected (GTK_DROP_DOWN (box->orientation));

  if (paper < G_N_ELEMENTS (PAPER_SIZES))
    {
      page.width  = PAPER_SIZES[paper][landscape == 1 ? 1 : 0];
      page.height = PAPER_SIZES[paper][landscape == 1 ? 0 : 1];
    }

  page.margin_top =
    (int) lround (twips_from_measure (gtk_spin_button_get_value (GTK_SPIN_BUTTON (box->top))));
  page.margin_bottom =
    (int) lround (twips_from_measure (gtk_spin_button_get_value (GTK_SPIN_BUTTON (box->bottom))));
  page.margin_left =
    (int) lround (twips_from_measure (gtk_spin_button_get_value (GTK_SPIN_BUTTON (box->left))));
  page.margin_right =
    (int) lround (twips_from_measure (gtk_spin_button_get_value (GTK_SPIN_BUTTON (box->right))));

  /* Margins that meet in the middle would leave a text column of zero width
   * and a document that cannot be laid out at all. */
  if (page.margin_left + page.margin_right >= page.width - 720 ||
      page.margin_top + page.margin_bottom >= page.height - 720)
    {
      w42_message_show (GTK_WINDOW (box->window),
                        "The margins leave no room for text: an inch or less "
                        "each, please.", NULL);
      return;
    }

  w42_document_set_page_setup (doc, &page);
  gtk_window_destroy (GTK_WINDOW (box->window));
}

void
w42_page_setup_dialog_show (GtkWindow *parent, W42View *view)
{
  PageSetupBox *box;
  GtkWidget *content, *margins, *paper;
  W42Document *doc;
  const W42PageSetup *page;
  guint paper_index = 0;
  gboolean landscape;

  g_return_if_fail (W42_IS_VIEW (view));

  doc = w42_view_get_document (view);
  if (doc == NULL)
    return;

  page = w42_document_page_setup (doc);
  landscape = page->width > page->height;

  /* A size not in the list stays as it is: the last entry. */
  paper_index = G_N_ELEMENTS (PAPER_SIZES);
  for (guint i = 0; i < G_N_ELEMENTS (PAPER_SIZES); i++)
    {
      int w = PAPER_SIZES[i][landscape ? 1 : 0];
      int h = PAPER_SIZES[i][landscape ? 0 : 1];

      if (ABS (page->width - w) < 60 && ABS (page->height - h) < 60)
        {
          paper_index = i;
          break;
        }
    }

  box = g_new0 (PageSetupBox, 1);
  box->view = view;
  box->window = dialog_shell (parent, "Page Setup", &content, view);
  g_object_weak_ref (G_OBJECT (box->window), page_setup_free, box);

  margins = group (content, "Margins");
  box->top    = inches_row (margins, 0, 0, "_Top:",    measure_from_twips (page->margin_top));
  box->bottom = inches_row (margins, 1, 0, "_Bottom:", measure_from_twips (page->margin_bottom));
  box->left   = inches_row (margins, 0, 1, "_Left:",   measure_from_twips (page->margin_left));
  box->right  = inches_row (margins, 1, 1, "_Right:",  measure_from_twips (page->margin_right));

  paper = group (content, "Paper");
  box->paper = choice_row (paper, 0, 0, "Paper _Size:", PAPER_NAMES, paper_index);
  box->orientation = choice_row (paper, 1, 0, "Orie_ntation:", ORIENTATIONS,
                                 landscape ? 1 : 0);

  button_row (content, box->window, G_CALLBACK (on_page_setup_ok), box);
  gtk_window_present (GTK_WINDOW (box->window));
}

/* ---------------------------------------------------------------------- */
/* Paragraph                                                               */
/* ---------------------------------------------------------------------- */

typedef struct {
  GtkWidget *window;
  W42View   *view;
  GtkWidget *align;
  GtkWidget *direction;
  GtkWidget *left, *right, *special, *by;
  GtkWidget *before, *after, *spacing;
  GtkWidget *keep_next, *keep_together, *widows;
  GtkWidget *at;
} ParagraphBox;

static const char * const ALIGNMENTS[] = {
  "Left", "Centered", "Right", "Justified", NULL
};

static const char * const DIRECTIONS[] = { "Left-to-right", "Right-to-left", NULL };

static const char * const SPECIALS[] = {
  "(none)", "First Line", "Hanging", NULL
};

static const char * const SPACINGS[] = {
  "Single", "1.5 Lines", "Double", "Exactly", "Multiple", NULL
};

static const int SPACING_PCT[] = { 100, 150, 200 };

static void
paragraph_free (gpointer data, GObject *gone)
{
  (void) gone;
  g_free (data);
}

static void
on_special_changed (GtkDropDown *drop, GParamSpec *pspec, gpointer data)
{
  ParagraphBox *box = data;

  (void) pspec;

  /* "By" only means something once you have said what it is by. */
  gtk_widget_set_sensitive (box->by,
                            gtk_drop_down_get_selected (drop) != 0);
}

static void
on_spacing_changed (GtkDropDown *drop, GParamSpec *pspec, gpointer data)
{
  ParagraphBox *box = data;

  (void) pspec;
  gtk_widget_set_sensitive (box->at, gtk_drop_down_get_selected (drop) >= 3);
}

static void
on_paragraph_ok (GtkButton *button, gpointer data)
{
  ParagraphBox *box = data;
  W42ParaFmt want;
  guint special, spacing;
  double by;

  (void) button;

  memset (&want, 0, sizeof want);

  want.align = (W42Align) gtk_drop_down_get_selected (GTK_DROP_DOWN (box->align));
  want.rtl = gtk_drop_down_get_selected (GTK_DROP_DOWN (box->direction)) == 1;
  want.indent_left =
    (int) lround (twips_from_measure (gtk_spin_button_get_value (GTK_SPIN_BUTTON (box->left))));
  want.indent_right =
    (int) lround (twips_from_measure (gtk_spin_button_get_value (GTK_SPIN_BUTTON (box->right))));

  special = gtk_drop_down_get_selected (GTK_DROP_DOWN (box->special));
  by = twips_from_measure (gtk_spin_button_get_value (GTK_SPIN_BUTTON (box->by)));

  /* A hanging indent is a negative first line: the first line starts left of
   * the rest of the paragraph. */
  if (special == 1)
    want.indent_first = (int) lround (by);
  else if (special == 2)
    want.indent_first = -(int) lround (by);
  else
    want.indent_first = 0;

  want.space_before = (int) lround (gtk_spin_button_get_value (GTK_SPIN_BUTTON (box->before)) * 20.0);
  want.space_after = (int) lround (gtk_spin_button_get_value (GTK_SPIN_BUTTON (box->after)) * 20.0);

  spacing = gtk_drop_down_get_selected (GTK_DROP_DOWN (box->spacing));
  if (spacing == 3)
    {
      /* Exactly: a leading in points, whatever the type size. */
      want.line_spacing_pct = 0;
      want.line_spacing = (int) lround (gtk_spin_button_get_value (GTK_SPIN_BUTTON (box->at)) * 20.0);
    }
  else if (spacing == 4)
    {
      want.line_spacing = 0;
      want.line_spacing_pct = (int) lround (gtk_spin_button_get_value (GTK_SPIN_BUTTON (box->at)) * 100.0);
    }
  else
    {
      want.line_spacing = 0;
      want.line_spacing_pct = (spacing < G_N_ELEMENTS (SPACING_PCT)) ? SPACING_PCT[spacing] : 100;
    }

  want.keep_next     = gtk_check_button_get_active (GTK_CHECK_BUTTON (box->keep_next)) ? 1 : 0;
  want.keep_together = gtk_check_button_get_active (GTK_CHECK_BUTTON (box->keep_together)) ? 1 : 0;
  want.widow_control = gtk_check_button_get_active (GTK_CHECK_BUTTON (box->widows)) ? 1 : 0;

  w42_view_apply_para_fmt (box->view,
                           W42_PARA_ALIGN | W42_PARA_INDENT_LEFT |
                           W42_PARA_INDENT_RIGHT | W42_PARA_INDENT_FIRST |
                           W42_PARA_SPACE_BEFORE | W42_PARA_SPACE_AFTER |
                           W42_PARA_LINE_SPACING | W42_PARA_LINE_SPACING_PCT |
                           W42_PARA_FLOW,
                           &want);

  gtk_window_destroy (GTK_WINDOW (box->window));
}

void
w42_paragraph_dialog_show (GtkWindow *parent, W42View *view)
{
  ParagraphBox *box;
  GtkWidget *content, *indent, *spacing_group;
  W42ParaFmt now;
  guint special = 0, spacing_index = 0;
  double by = 0.0;

  g_return_if_fail (W42_IS_VIEW (view));

  if (w42_view_get_document (view) == NULL)
    return;

  w42_view_get_para_fmt (view, &now);

  if (now.indent_first > 0)
    {
      special = 1;
      by = measure_from_twips (now.indent_first);
    }
  else if (now.indent_first < 0)
    {
      special = 2;
      by = measure_from_twips (-now.indent_first);
    }

  for (guint i = 0; i < G_N_ELEMENTS (SPACING_PCT); i++)
    if (now.line_spacing_pct == SPACING_PCT[i])
      spacing_index = i;
  if (now.line_spacing > 0 && now.line_spacing_pct == 0)
    spacing_index = 3;               /* Exactly */
  else if (now.line_spacing_pct > 0 && now.line_spacing_pct != 100 && now.line_spacing_pct != 150 && now.line_spacing_pct != 200)
    spacing_index = 4;               /* Multiple */

  box = g_new0 (ParagraphBox, 1);
  box->view = view;
  box->window = dialog_shell (parent, "Paragraph", &content, view);
  g_object_weak_ref (G_OBJECT (box->window), paragraph_free, box);

  indent = group (content, "Indentation");
  box->align = choice_row (indent, 0, 0, "_Alignment:", ALIGNMENTS,
                           (guint) now.align);
  box->direction = choice_row (indent, 0, 1, "_Direction:", DIRECTIONS, now.rtl ? 1 : 0);
  box->left  = inches_row (indent, 1, 0, "_Left:",
                           measure_from_twips (now.indent_left));
  box->right = inches_row (indent, 2, 0, "_Right:",
                           measure_from_twips (now.indent_right));
  box->special = choice_row (indent, 1, 1, "_Special:", SPECIALS, special);
  box->by = inches_row (indent, 2, 1, "B_y:", by);
  gtk_widget_set_sensitive (box->by, special != 0);
  g_signal_connect (box->special, "notify::selected",
                    G_CALLBACK (on_special_changed), box);

  spacing_group = group (content, "Spacing");
  /* Before and After in points, as the Style box and the classics had
   * them: six points is a typical value and a tenth of an inch is not. */
  box->before = inches_row (spacing_group, 0, 0, "Be_fore (pt):", now.space_before / 20.0);
  box->after  = inches_row (spacing_group, 1, 0, "After (_pt):", now.space_after / 20.0);
  for (int k = 0; k < 2; k++)
    {
      GtkSpinButton *sp = GTK_SPIN_BUTTON (k == 0 ? box->before : box->after);

      gtk_spin_button_set_range (sp, 0, 720);
      gtk_spin_button_set_increments (sp, 6, 12);
      gtk_spin_button_set_digits (sp, 0);
      gtk_spin_button_set_value (sp, (k == 0 ? now.space_before : now.space_after) / 20.0);
    }
  box->spacing = choice_row (spacing_group, 0, 1, "Li_ne Spacing:",
                             SPACINGS, spacing_index);
  {
    /* "At:" -- points for Exactly, a multiple of the line for Multiple. */
    double at = now.line_spacing_pct > 0 && spacing_index == 4 ? now.line_spacing_pct / 100.0
              : now.line_spacing > 0 ? now.line_spacing / 20.0 : 12.0;

    box->at = inches_row (spacing_group, 1, 1, "A_t:", at);
    gtk_spin_button_set_range (GTK_SPIN_BUTTON (box->at), 0.5, 720);
    gtk_spin_button_set_increments (GTK_SPIN_BUTTON (box->at), 0.5, 6);
    gtk_spin_button_set_digits (GTK_SPIN_BUTTON (box->at), 1);
    gtk_spin_button_set_value (GTK_SPIN_BUTTON (box->at), at);
    gtk_widget_set_sensitive (box->at, spacing_index >= 3);
    g_signal_connect (box->spacing, "notify::selected", G_CALLBACK (on_spacing_changed), box);
  }

  {
    GtkWidget *flow = group (content, "Text Flow");

    box->widows        = gtk_check_button_new_with_mnemonic ("_Widow/Orphan Control");
    box->keep_together = gtk_check_button_new_with_mnemonic ("_Keep Lines Together");
    box->keep_next     = gtk_check_button_new_with_mnemonic ("Keep with Ne_xt");
    gtk_check_button_set_active (GTK_CHECK_BUTTON (box->widows), now.widow_control);
    gtk_check_button_set_active (GTK_CHECK_BUTTON (box->keep_together), now.keep_together);
    gtk_check_button_set_active (GTK_CHECK_BUTTON (box->keep_next), now.keep_next);
    gtk_grid_attach (GTK_GRID (flow), box->widows, 0, 0, 1, 1);
    gtk_grid_attach (GTK_GRID (flow), box->keep_together, 1, 0, 1, 1);
    gtk_grid_attach (GTK_GRID (flow), box->keep_next, 0, 1, 1, 1);
  }

  button_row (content, box->window, G_CALLBACK (on_paragraph_ok), box);
  gtk_window_present (GTK_WINDOW (box->window));
}

/* ---------------------------------------------------------------------- */
/* Style                                                                   */
/* ---------------------------------------------------------------------- */

typedef struct {
  GtkWidget *window;
  W42View   *view;
  GtkWidget *styles;       /* the drop-down of names */
  GtkStringList *names;
  GtkWidget *family, *size, *bold, *italic, *align, *before, *after, *outline;
  GtkWidget *kind;         /* "Paragraph style, based on X" */
  GtkWidget *delete_button;
  gboolean   loading;
} StyleBox;

static void
style_free (gpointer data, GObject *gone)
{
  (void) gone;
  g_free (data);
}

static W42StyleSheet *
style_box_sheet (StyleBox *box)
{
  return w42_pt_stylesheet (w42_document_pt (w42_view_get_document (box->view)));
}

static const W42Style *
style_box_current (StyleBox *box)
{
  guint index = gtk_drop_down_get_selected (GTK_DROP_DOWN (box->styles));

  return w42_stylesheet_get (style_box_sheet (box), index);
}

/* The fields show the selected style's definition. */
static void
style_box_load (StyleBox *box)
{
  const W42Style *style = style_box_current (box);

  if (style == NULL)
    return;

  box->loading = TRUE;
  gtk_editable_set_text (GTK_EDITABLE (box->family),
                         style->ch.family != NULL ? style->ch.family : "");
  gtk_spin_button_set_value (GTK_SPIN_BUTTON (box->size), style->ch.size / 2.0);
  gtk_check_button_set_active (GTK_CHECK_BUTTON (box->bold), style->ch.bold);
  gtk_check_button_set_active (GTK_CHECK_BUTTON (box->italic), style->ch.italic);
  gtk_drop_down_set_selected (GTK_DROP_DOWN (box->align), (guint) style->pa.align);
  gtk_spin_button_set_value (GTK_SPIN_BUTTON (box->before),
                             style->pa.space_before / 20.0);
  gtk_spin_button_set_value (GTK_SPIN_BUTTON (box->after),
                             style->pa.space_after / 20.0);
  gtk_spin_button_set_value (GTK_SPIN_BUTTON (box->outline), style->outline);

  /* A character style has no paragraph half to edit. */
  gtk_widget_set_sensitive (box->align, !style->character);
  gtk_widget_set_sensitive (box->before, !style->character);
  gtk_widget_set_sensitive (box->after, !style->character);
  gtk_widget_set_sensitive (box->outline, !style->character);
  gtk_widget_set_sensitive (box->delete_button, g_ascii_strcasecmp (style->name, "Normal") != 0);
  {
    char *text = style->based_on != NULL
      ? g_strdup_printf ("%s style, based on %s", style->character ? "Character" : "Paragraph", style->based_on)
      : g_strdup_printf ("%s style", style->character ? "Character" : "Paragraph");

    gtk_label_set_text (GTK_LABEL (box->kind), text);
    g_free (text);
  }
  box->loading = FALSE;
}

static void style_box_store (StyleBox *box);

/* New: a style made from the selected one, under a name of its own. */
typedef struct {
  StyleBox  *box;
  GtkWidget *window;
  GtkWidget *name;
  GtkWidget *character;
} NewStyleBox;

static void
on_new_style_ok (GtkButton *button, gpointer data)
{
  NewStyleBox *nb = data;
  StyleBox *box = nb->box;
  const W42Style *base = style_box_current (box);
  const char *name = gtk_editable_get_text (GTK_EDITABLE (nb->name));
  W42StyleSheet *sheet = style_box_sheet (box);
  W42Style style;

  (void) button;
  if (base == NULL || name == NULL || *name == '\0' ||
      w42_stylesheet_find (sheet, name) != NULL)
    {
      gtk_widget_grab_focus (nb->name);
      return;
    }

  style_box_store (box);               /* the base as it stands in the fields */
  base = style_box_current (box);      /* the store replaced the entry */
  if (base == NULL)
    return;
  style = *base;
  style.name = g_intern_string (name);
  style.pa.style = style.name;
  style.based_on = base->name;
  style.character = gtk_check_button_get_active (GTK_CHECK_BUTTON (nb->character)) ? 1 : 0;
  if (style.character)
    style.outline = 0;
  /* Nothing of its own yet: it follows its base until edited. */
  style.pa_own = 0;
  style.ch_own = 0;
  w42_stylesheet_set (sheet, &style);

  gtk_string_list_append (box->names, style.name);
  gtk_drop_down_set_selected (GTK_DROP_DOWN (box->styles), w42_stylesheet_size (sheet) - 1);
  w42_document_mark_unsaved (w42_view_get_document (box->view));
  gtk_window_destroy (GTK_WINDOW (nb->window));
}

static void
on_style_new (GtkButton *button, gpointer data)
{
  StyleBox *box = data;
  NewStyleBox *nb = g_new0 (NewStyleBox, 1);
  GtkWidget *content, *grid, *label;

  (void) button;
  nb->box = box;
  nb->window = dialog_shell (GTK_WINDOW (box->window), "New Style", &content, box->view);
  g_object_weak_ref (G_OBJECT (nb->window), style_free, nb);

  grid = group (content, "New style");
  label = gtk_label_new_with_mnemonic ("_Name:");
  nb->name = gtk_entry_new ();
  gtk_label_set_xalign (GTK_LABEL (label), 0.0);
  gtk_label_set_mnemonic_widget (GTK_LABEL (label), nb->name);
  gtk_entry_set_activates_default (GTK_ENTRY (nb->name), TRUE);
  gtk_widget_set_size_request (nb->name, 200, -1);
  gtk_grid_attach (GTK_GRID (grid), label, 0, 0, 1, 1);
  gtk_grid_attach (GTK_GRID (grid), nb->name, 1, 0, 1, 1);
  nb->character = gtk_check_button_new_with_mnemonic ("_Character style (applies to selected text only)");
  gtk_grid_attach (GTK_GRID (grid), nb->character, 0, 1, 2, 1);
  {
    const W42Style *base = style_box_current (box);
    char *text = g_strdup_printf ("Based on %s.", base != NULL ? base->name : "Normal");

    label = gtk_label_new (text);
    gtk_label_set_xalign (GTK_LABEL (label), 0.0);
    gtk_grid_attach (GTK_GRID (grid), label, 0, 2, 2, 1);
    g_free (text);
  }

  button_row (content, nb->window, G_CALLBACK (on_new_style_ok), nb);
  gtk_window_present (GTK_WINDOW (nb->window));
  gtk_widget_grab_focus (nb->name);
}

static void
on_style_delete (GtkButton *button, gpointer data)
{
  StyleBox *box = data;
  const W42Style *style = style_box_current (box);
  W42PieceTable *pt = w42_document_pt (w42_view_get_document (box->view));
  guint index = gtk_drop_down_get_selected (GTK_DROP_DOWN (box->styles));
  const char *name;

  (void) button;
  if (style == NULL || g_ascii_strcasecmp (style->name, "Normal") == 0)
    return;
  name = style->name;                  /* interned: outlives the entry */
  if (!style->character)
    w42_pt_replace_style (pt, name, "Normal");
  w42_stylesheet_remove (style_box_sheet (box), name);
  gtk_string_list_remove (box->names, index);
  gtk_drop_down_set_selected (GTK_DROP_DOWN (box->styles), 0);
  w42_document_mark_unsaved (w42_view_get_document (box->view));
  w42_document_touch (w42_view_get_document (box->view));
}

static void
on_style_choice (GtkDropDown *drop, GParamSpec *pspec, gpointer data)
{
  (void) drop; (void) pspec;
  style_box_load (data);
}

/* Writes the fields back into the stylesheet and re-styles every paragraph
 * that uses the style, so the document follows the definition. */
static void
style_box_store (StyleBox *box)
{
  const W42Style *current = style_box_current (box);
  W42Style style;
  W42PieceTable *pt = w42_document_pt (w42_view_get_document (box->view));

  if (current == NULL)
    return;

  style = *current;
  style.ch.family = g_intern_string (gtk_editable_get_text (GTK_EDITABLE (box->family)));
  style.ch.size   = (int) lround (gtk_spin_button_get_value (GTK_SPIN_BUTTON (box->size)) * 2.0);
  style.ch.bold   = gtk_check_button_get_active (GTK_CHECK_BUTTON (box->bold)) ? 1 : 0;
  style.ch.italic = gtk_check_button_get_active (GTK_CHECK_BUTTON (box->italic)) ? 1 : 0;
  style.pa.align  = (W42Align) gtk_drop_down_get_selected (GTK_DROP_DOWN (box->align));
  style.pa.space_before = (int) lround (gtk_spin_button_get_value (GTK_SPIN_BUTTON (box->before)) * 20.0);
  style.pa.space_after  = (int) lround (gtk_spin_button_get_value (GTK_SPIN_BUTTON (box->after)) * 20.0);
  style.outline   = (int) gtk_spin_button_get_value (GTK_SPIN_BUTTON (box->outline));

  /* Nothing changed: leave the paragraphs be, since restyling them puts
   * the style's character formatting over any of their own. */
  if (memcmp (&style, current, sizeof style) == 0)
    return;

  /* What differs from the base is the style's own; the rest follows the
   * base, now and when the base changes. */
  {
    const W42Style *base = style.based_on != NULL ? w42_stylesheet_find (style_box_sheet (box), style.based_on) : NULL;
    guint32 pa_own, ch_own;

    w42_style_own_from_base (&style, base, &pa_own, &ch_own);
    style.pa_own = pa_own | (style.pa_own & ~W42_STYLE_PA_ALL);
    style.ch_own = ch_own;
  }
  w42_stylesheet_set (style_box_sheet (box), &style);
  w42_stylesheet_follow (style_box_sheet (box), style.name);
  w42_pt_restyle_tree (pt, style.name);
  w42_document_mark_unsaved (w42_view_get_document (box->view));
  w42_document_touch (w42_view_get_document (box->view));
}

static void
on_style_apply (GtkButton *button, gpointer data)
{
  StyleBox *box = data;
  const W42Style *style = style_box_current (box);
  const char *name = style != NULL ? style->name : NULL;   /* interned: survives the store */

  (void) button;

  style_box_store (box);
  if (name != NULL)
    w42_view_apply_style (box->view, name);
}

static void
on_style_ok (GtkButton *button, gpointer data)
{
  StyleBox *box = data;

  (void) button;
  style_box_store (box);
  gtk_window_destroy (GTK_WINDOW (box->window));
}

void
w42_style_dialog_show (GtkWindow *parent, W42View *view)
{
  StyleBox *box;
  GtkWidget *content, *names, *fields, *buttons, *apply;
  GtkStringList *list = gtk_string_list_new (NULL);
  W42StyleSheet *sheet;
  const char *current;
  static const char * const aligns[] = { "Left", "Centered", "Right", "Justified", NULL };

  g_return_if_fail (W42_IS_VIEW (view));

  if (w42_view_get_document (view) == NULL)
    return;

  sheet = w42_pt_stylesheet (w42_document_pt (w42_view_get_document (view)));
  current = w42_view_get_style (view);

  box = g_new0 (StyleBox, 1);
  box->view = view;
  box->window = dialog_shell (parent, "Style", &content, view);
  g_object_weak_ref (G_OBJECT (box->window), style_free, box);

  for (guint i = 0; i < w42_stylesheet_size (sheet); i++)
    gtk_string_list_append (list, w42_stylesheet_get (sheet, i)->name);

  names = group (content, "Styles");
  box->styles = choice_row (names, 0, 0, "_Style:", NULL, 0);
  box->names = list;
  gtk_drop_down_set_model (GTK_DROP_DOWN (box->styles), G_LIST_MODEL (list));
  {
    GtkWidget *row = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 8);
    GtkWidget *new_button = gtk_button_new_with_mnemonic ("_New...");

    box->delete_button = gtk_button_new_with_mnemonic ("_Delete");
    g_signal_connect (new_button, "clicked", G_CALLBACK (on_style_new), box);
    g_signal_connect (box->delete_button, "clicked", G_CALLBACK (on_style_delete), box);
    gtk_box_append (GTK_BOX (row), new_button);
    gtk_box_append (GTK_BOX (row), box->delete_button);
    gtk_grid_attach (GTK_GRID (names), row, 0, 1, 2, 1);
    box->kind = gtk_label_new ("");
    gtk_label_set_xalign (GTK_LABEL (box->kind), 0.0);
    gtk_widget_add_css_class (box->kind, "dim-label");
    gtk_grid_attach (GTK_GRID (names), box->kind, 0, 2, 2, 1);
  }
  for (guint i = 0; i < w42_stylesheet_size (sheet); i++)
    if (g_ascii_strcasecmp (w42_stylesheet_get (sheet, i)->name, current) == 0)
      gtk_drop_down_set_selected (GTK_DROP_DOWN (box->styles), i);

  fields = group (content, "Definition");
  {
    GtkWidget *label = gtk_label_new_with_mnemonic ("_Font:");
    box->family = gtk_entry_new ();
    gtk_label_set_xalign (GTK_LABEL (label), 0.0);
    gtk_label_set_mnemonic_widget (GTK_LABEL (label), box->family);
    gtk_widget_set_size_request (box->family, 160, -1);
    gtk_grid_attach (GTK_GRID (fields), label, 0, 0, 1, 1);
    gtk_grid_attach (GTK_GRID (fields), box->family, 1, 0, 1, 1);

    label = gtk_label_new_with_mnemonic ("Si_ze (pt):");
    box->size = gtk_spin_button_new_with_range (4, 144, 1);
    gtk_label_set_xalign (GTK_LABEL (label), 0.0);
    gtk_label_set_mnemonic_widget (GTK_LABEL (label), box->size);
    gtk_grid_attach (GTK_GRID (fields), label, 2, 0, 1, 1);
    gtk_grid_attach (GTK_GRID (fields), box->size, 3, 0, 1, 1);

    box->bold = gtk_check_button_new_with_mnemonic ("_Bold");
    box->italic = gtk_check_button_new_with_mnemonic ("_Italic");
    gtk_grid_attach (GTK_GRID (fields), box->bold, 1, 1, 1, 1);
    gtk_grid_attach (GTK_GRID (fields), box->italic, 3, 1, 1, 1);
  }
  box->align = choice_row (fields, 2, 0, "Ali_gnment:", aligns, 0);
  box->before = inches_row (fields, 3, 0, "Space B_efore (pt):", 0.0);
  box->after = inches_row (fields, 3, 1, "Space A_fter (pt):", 0.0);
  gtk_spin_button_set_range (GTK_SPIN_BUTTON (box->before), 0, 200);
  gtk_spin_button_set_range (GTK_SPIN_BUTTON (box->after), 0, 200);
  gtk_spin_button_set_increments (GTK_SPIN_BUTTON (box->before), 6, 12);
  gtk_spin_button_set_increments (GTK_SPIN_BUTTON (box->after), 6, 12);
  gtk_spin_button_set_digits (GTK_SPIN_BUTTON (box->before), 0);
  gtk_spin_button_set_digits (GTK_SPIN_BUTTON (box->after), 0);
  {
    GtkWidget *label = gtk_label_new_with_mnemonic ("Outline le_vel:");
    box->outline = gtk_spin_button_new_with_range (0, 9, 1);
    gtk_label_set_xalign (GTK_LABEL (label), 0.0);
    gtk_label_set_mnemonic_widget (GTK_LABEL (label), box->outline);
    gtk_widget_set_tooltip_text (box->outline,
      "0 for body text; 1 to 9 for a heading of that level, which is what "
      "Heading Numbering counts.");
    gtk_grid_attach (GTK_GRID (fields), label, 0, 4, 1, 1);
    gtk_grid_attach (GTK_GRID (fields), box->outline, 1, 4, 1, 1);
  }

  g_signal_connect (box->styles, "notify::selected",
                    G_CALLBACK (on_style_choice), box);
  style_box_load (box);

  buttons = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 8);
  gtk_widget_set_halign (buttons, GTK_ALIGN_END);
  apply = gtk_button_new_with_mnemonic ("_Apply");
  gtk_widget_set_size_request (apply, 92, 26);
  gtk_widget_set_tooltip_text (apply, "Save the definition and apply the "
                               "style to the selected paragraphs.");
  g_signal_connect (apply, "clicked", G_CALLBACK (on_style_apply), box);
  gtk_box_append (GTK_BOX (buttons), apply);
  gtk_box_append (GTK_BOX (content), buttons);
  {
    GtkWidget *ok = gtk_button_new_with_mnemonic ("_OK");
    GtkWidget *cancel = gtk_button_new_with_mnemonic ("Cancel");
    gtk_widget_set_size_request (ok, 92, 26);
    gtk_widget_set_size_request (cancel, 92, 26);
    g_signal_connect (ok, "clicked", G_CALLBACK (on_style_ok), box);
    g_signal_connect_swapped (cancel, "clicked", G_CALLBACK (gtk_window_destroy), box->window);
    gtk_box_append (GTK_BOX (buttons), ok);
    gtk_box_append (GTK_BOX (buttons), cancel);
    gtk_window_set_default_widget (GTK_WINDOW (box->window), ok);
  }

  gtk_window_present (GTK_WINDOW (box->window));

  /* Focus starts on the list of styles, which is what the box is about.
   * Left to itself GTK picks the last spinner, and a stray arrow key there
   * quietly changes a heading's outline level. */
  gtk_widget_grab_focus (box->styles);
}

/* ---------------------------------------------------------------------- */
/* Header and Footer                                                       */
/* ---------------------------------------------------------------------- */

typedef struct {
  GtkWidget *window;
  W42View   *view;
  GtkWidget *header, *header_align;
  GtkWidget *footer, *footer_align;
  /* The first page and the even pages may have their own, as Word's
   * "Different First Page" and "Different Odd and Even Pages" do. */
  GtkWidget *title_page, *facing_pages;
  GtkWidget *first_group, *even_group;
  GtkWidget *first_header, *first_header_align;
  GtkWidget *first_footer, *first_footer_align;
  GtkWidget *even_header, *even_header_align;
  GtkWidget *even_footer, *even_footer_align;
} HeaderFooterBox;

static const char * const HF_ALIGNS[] = { "Left", "Center", "Right", NULL };

static void
hf_free (gpointer data, GObject *gone)
{
  (void) gone;
  g_free (data);
}

static W42Align
hf_align_from (GtkWidget *drop)
{
  switch (gtk_drop_down_get_selected (GTK_DROP_DOWN (drop)))
    {
    case 1:  return W42_ALIGN_CENTER;
    case 2:  return W42_ALIGN_RIGHT;
    default: return W42_ALIGN_LEFT;
    }
}

static guint
hf_align_index (W42Align align)
{
  return align == W42_ALIGN_CENTER ? 1 : align == W42_ALIGN_RIGHT ? 2 : 0;
}

static void
on_hf_ok (GtkButton *button, gpointer data)
{
  HeaderFooterBox *box = data;
  W42Document *doc = w42_view_get_document (box->view);
  W42PieceTable *pt = w42_document_pt (doc);

  (void) button;

  w42_pt_set_header (pt, gtk_editable_get_text (GTK_EDITABLE (box->header)),
                     hf_align_from (box->header_align));
  w42_pt_set_footer (pt, gtk_editable_get_text (GTK_EDITABLE (box->footer)),
                     hf_align_from (box->footer_align));

  w42_pt_set_title_page (pt, gtk_check_button_get_active (GTK_CHECK_BUTTON (box->title_page)));
  w42_pt_set_facing_pages (pt, gtk_check_button_get_active (GTK_CHECK_BUTTON (box->facing_pages)));
  w42_pt_set_header_kind (pt, W42_PAGE_TEXT_FIRST,
                          gtk_editable_get_text (GTK_EDITABLE (box->first_header)),
                          hf_align_from (box->first_header_align));
  w42_pt_set_footer_kind (pt, W42_PAGE_TEXT_FIRST,
                          gtk_editable_get_text (GTK_EDITABLE (box->first_footer)),
                          hf_align_from (box->first_footer_align));
  w42_pt_set_header_kind (pt, W42_PAGE_TEXT_EVEN,
                          gtk_editable_get_text (GTK_EDITABLE (box->even_header)),
                          hf_align_from (box->even_header_align));
  w42_pt_set_footer_kind (pt, W42_PAGE_TEXT_EVEN,
                          gtk_editable_get_text (GTK_EDITABLE (box->even_footer)),
                          hf_align_from (box->even_footer_align));

  w42_document_mark_unsaved (doc);
  w42_document_touch (doc);
  gtk_window_destroy (GTK_WINDOW (box->window));
}

static GtkWidget *hf_row (GtkWidget *grid, int row, const char *label, const char *text,
                          GtkWidget **align_out, W42Align align);

/* A page kind nobody has asked for is shown greyed, so the box says what
 * it can do without letting you type into rows that would not be used. */
static void
on_hf_kind_toggled (GtkCheckButton *check, gpointer data)
{
  gtk_widget_set_sensitive (GTK_WIDGET (data), gtk_check_button_get_active (check));
}

static GtkWidget *
hf_kind_group (GtkWidget *content, HeaderFooterBox *box, const char *title,
               const char *switch_label, W42PageTextKind kind, gboolean on,
               GtkWidget **check_out,
               GtkWidget **header_out, GtkWidget **header_align_out,
               GtkWidget **footer_out, GtkWidget **footer_align_out)
{
  W42PieceTable *pt = w42_document_pt (w42_view_get_document (box->view));
  const W42PageText *header = w42_pt_get_header_kind (pt, kind);
  const W42PageText *footer = w42_pt_get_footer_kind (pt, kind);
  GtkWidget *check = gtk_check_button_new_with_mnemonic (switch_label);
  GtkWidget *grid;

  /* The switch first, then the rows it governs, in that order down the box. */
  gtk_box_append (GTK_BOX (content), check);
  grid = group (content, title);

  gtk_check_button_set_active (GTK_CHECK_BUTTON (check), on);
  gtk_widget_set_sensitive (grid, on);
  g_signal_connect (check, "toggled", G_CALLBACK (on_hf_kind_toggled), grid);

  *header_out = hf_row (grid, 0, "H_eader:", header->text, header_align_out, header->align);
  *footer_out = hf_row (grid, 1, "F_ooter:", footer->text, footer_align_out, footer->align);
  *check_out = check;
  return grid;
}

static GtkWidget *
hf_row (GtkWidget *grid, int row, const char *label, const char *text,
        GtkWidget **align_out, W42Align align)
{
  GtkWidget *name = gtk_label_new_with_mnemonic (label);
  GtkWidget *entry = gtk_entry_new ();
  GtkWidget *drop = gtk_drop_down_new_from_strings (HF_ALIGNS);

  gtk_label_set_xalign (GTK_LABEL (name), 0.0);
  gtk_label_set_mnemonic_widget (GTK_LABEL (name), entry);
  gtk_widget_set_hexpand (entry, TRUE);
  gtk_widget_set_size_request (entry, 260, -1);
  gtk_editable_set_text (GTK_EDITABLE (entry), text != NULL ? text : "");
  gtk_entry_set_activates_default (GTK_ENTRY (entry), TRUE);
  gtk_drop_down_set_selected (GTK_DROP_DOWN (drop), hf_align_index (align));

  gtk_grid_attach (GTK_GRID (grid), name, 0, row, 1, 1);
  gtk_grid_attach (GTK_GRID (grid), entry, 1, row, 1, 1);
  gtk_grid_attach (GTK_GRID (grid), drop, 2, row, 1, 1);

  *align_out = drop;
  return entry;
}

void
w42_header_footer_dialog_show (GtkWindow *parent, W42View *view)
{
  HeaderFooterBox *box;
  GtkWidget *content, *grid, *hint;
  W42PieceTable *pt;
  const W42PageText *header, *footer;

  g_return_if_fail (W42_IS_VIEW (view));

  if (w42_view_get_document (view) == NULL)
    return;

  pt = w42_document_pt (w42_view_get_document (view));
  header = w42_pt_get_header (pt);
  footer = w42_pt_get_footer (pt);

  box = g_new0 (HeaderFooterBox, 1);
  box->view = view;
  box->window = dialog_shell (parent, "Header and Footer", &content, view);
  g_object_weak_ref (G_OBJECT (box->window), hf_free, box);

  grid = group (content, "Text");
  box->header = hf_row (grid, 0, "_Header:", header->text, &box->header_align, header->align);
  box->footer = hf_row (grid, 1, "_Footer:", footer->text, &box->footer_align, footer->align);

  box->first_group = hf_kind_group (content, box, "First Page",
                                    "_Different first page", W42_PAGE_TEXT_FIRST,
                                    w42_pt_get_title_page (pt), &box->title_page,
                                    &box->first_header, &box->first_header_align,
                                    &box->first_footer, &box->first_footer_align);
  box->even_group = hf_kind_group (content, box, "Even Pages",
                                   "Different odd and _even pages", W42_PAGE_TEXT_EVEN,
                                   w42_pt_get_facing_pages (pt), &box->facing_pages,
                                   &box->even_header, &box->even_header_align,
                                   &box->even_footer, &box->even_footer_align);

  hint = gtk_label_new ("Fields: {PAGE} is the page number, {NUMPAGES} the "
                        "number of pages, {DATE} today's date.  Headers and "
                        "footers show in Page Layout view and in print.");
  gtk_label_set_wrap (GTK_LABEL (hint), TRUE);
  gtk_label_set_max_width_chars (GTK_LABEL (hint), 60);
  gtk_label_set_xalign (GTK_LABEL (hint), 0.0);
  gtk_widget_add_css_class (hint, "w42-dialog-status");
  gtk_box_append (GTK_BOX (content), hint);

  button_row (content, box->window, G_CALLBACK (on_hf_ok), box);
  gtk_window_present (GTK_WINDOW (box->window));
  gtk_widget_grab_focus (box->header);
}

/* ---------------------------------------------------------------------- */
/* Page Numbers                                                            */
/* ---------------------------------------------------------------------- */

typedef struct {
  GtkWidget *window;
  W42View   *view;
  GtkWidget *position, *align;
} PageNumbersBox;

static void
on_page_numbers_ok (GtkButton *button, gpointer data)
{
  PageNumbersBox *box = data;
  W42Document *doc = w42_view_get_document (box->view);
  W42PieceTable *pt = w42_document_pt (doc);
  gboolean top = gtk_drop_down_get_selected (GTK_DROP_DOWN (box->position)) == 0;
  W42Align align = hf_align_from (box->align);

  (void) button;

  /* The number joins a header or footer already there rather than
   * replacing it. */
  {
    const W42PageText *have = top ? w42_pt_get_header (pt) : w42_pt_get_footer (pt);
    char *text;

    if (have != NULL && have->text != NULL && *have->text != '\0' && strstr (have->text, "{PAGE}") == NULL)
      text = g_strdup_printf ("%s {PAGE}", have->text);
    else if (have != NULL && have->text != NULL && strstr (have->text, "{PAGE}") != NULL)
      text = g_strdup (have->text);
    else
      text = g_strdup ("{PAGE}");
    if (top)
      w42_pt_set_header (pt, text, align);
    else
      w42_pt_set_footer (pt, text, align);
    g_free (text);
  }

  w42_document_mark_unsaved (doc);
  w42_document_touch (doc);
  gtk_window_destroy (GTK_WINDOW (box->window));
}

void
w42_page_numbers_dialog_show (GtkWindow *parent, W42View *view)
{
  PageNumbersBox *box;
  GtkWidget *content, *grid;
  static const char * const positions[] = { "Top of Page (Header)", "Bottom of Page (Footer)", NULL };

  g_return_if_fail (W42_IS_VIEW (view));

  if (w42_view_get_document (view) == NULL)
    return;

  box = g_new0 (PageNumbersBox, 1);
  box->view = view;
  box->window = dialog_shell (parent, "Page Numbers", &content, view);
  g_object_weak_ref (G_OBJECT (box->window), hf_free, box);

  grid = group (content, "Position");
  box->position = choice_row (grid, 0, 0, "_Position:", positions, 1);
  box->align = choice_row (grid, 1, 0, "_Alignment:", HF_ALIGNS, 1);

  button_row (content, box->window, G_CALLBACK (on_page_numbers_ok), box);
  gtk_window_present (GTK_WINDOW (box->window));
  gtk_widget_grab_focus (box->position);
}

/* ---------------------------------------------------------------------- */
/* Insert Table                                                            */
/* ---------------------------------------------------------------------- */

typedef struct {
  GtkWidget *window;
  W42View   *view;
  GtkWidget *cols, *rows;
} InsertTableBox;

static void
on_insert_table_ok (GtkButton *button, gpointer data)
{
  InsertTableBox *box = data;

  (void) button;

  w42_view_insert_table (box->view,
                         (int) gtk_spin_button_get_value (GTK_SPIN_BUTTON (box->rows)),
                         (int) gtk_spin_button_get_value (GTK_SPIN_BUTTON (box->cols)));
  gtk_window_destroy (GTK_WINDOW (box->window));
}

void
w42_insert_table_dialog_show (GtkWindow *parent, W42View *view)
{
  InsertTableBox *box;
  GtkWidget *content, *grid, *label;

  g_return_if_fail (W42_IS_VIEW (view));

  if (w42_view_get_document (view) == NULL)
    return;

  box = g_new0 (InsertTableBox, 1);
  box->view = view;
  box->window = dialog_shell (parent, "Insert Table", &content, view);
  g_object_weak_ref (G_OBJECT (box->window), hf_free, box);

  grid = group (content, "Table Size");

  label = gtk_label_new_with_mnemonic ("Number of _Columns:");
  box->cols = gtk_spin_button_new_with_range (1, 20, 1);
  gtk_spin_button_set_value (GTK_SPIN_BUTTON (box->cols), 2);
  gtk_label_set_xalign (GTK_LABEL (label), 0.0);
  gtk_label_set_mnemonic_widget (GTK_LABEL (label), box->cols);
  gtk_grid_attach (GTK_GRID (grid), label, 0, 0, 1, 1);
  gtk_grid_attach (GTK_GRID (grid), box->cols, 1, 0, 1, 1);

  label = gtk_label_new_with_mnemonic ("Number of _Rows:");
  box->rows = gtk_spin_button_new_with_range (1, 200, 1);
  gtk_spin_button_set_value (GTK_SPIN_BUTTON (box->rows), 2);
  gtk_spin_button_set_activates_default (GTK_SPIN_BUTTON (box->rows), TRUE);
  gtk_spin_button_set_activates_default (GTK_SPIN_BUTTON (box->cols), TRUE);
  gtk_label_set_xalign (GTK_LABEL (label), 0.0);
  gtk_label_set_mnemonic_widget (GTK_LABEL (label), box->rows);
  gtk_grid_attach (GTK_GRID (grid), label, 0, 1, 1, 1);
  gtk_grid_attach (GTK_GRID (grid), box->rows, 1, 1, 1, 1);

  button_row (content, box->window, G_CALLBACK (on_insert_table_ok), box);
  gtk_window_present (GTK_WINDOW (box->window));
  gtk_widget_grab_focus (box->cols);
}

/* ---------------------------------------------------------------------- */
/* Go To                                                                   */
/* ---------------------------------------------------------------------- */

typedef struct {
  GtkWidget *window;
  W42View   *view;
  GtkWidget *what;      /* Page, Line or Bookmark */
  GtkWidget *number;
  GtkWidget *names;     /* the bookmarks, shown for Bookmark */
  char     **bookmarks;
} GoToBox;

static void
go_to_free (gpointer data, GObject *gone)
{
  GoToBox *box = data;

  (void) gone;
  g_strfreev (box->bookmarks);
  g_free (box);
}

static const char * const GO_TO_KINDS[] = { "Page", "Line", "Bookmark", NULL };

static void
on_go_to_ok (GtkButton *button, gpointer data)
{
  GoToBox *box = data;
  int n = (int) gtk_spin_button_get_value (GTK_SPIN_BUTTON (box->number));
  guint kind = gtk_drop_down_get_selected (GTK_DROP_DOWN (box->what));

  (void) button;

  if (kind == 0)
    w42_view_go_to_page (box->view, n);
  else if (kind == 1)
    w42_view_go_to_line (box->view, n);
  else
    {
      guint i = gtk_drop_down_get_selected (GTK_DROP_DOWN (box->names));

      if (box->bookmarks != NULL && i != GTK_INVALID_LIST_POSITION &&
          i < g_strv_length (box->bookmarks))
        w42_view_go_to_bookmark (box->view, box->bookmarks[i]);
    }

  gtk_window_destroy (GTK_WINDOW (box->window));
}

static void
on_go_to_kind (GObject *drop, GParamSpec *pspec, gpointer data)
{
  GoToBox *box = data;
  W42Layout *layout = w42_view_get_layout (box->view);
  guint kind = gtk_drop_down_get_selected (GTK_DROP_DOWN (drop));
  int top = kind == 0 ? w42_layout_n_pages (layout)
                      : w42_view_line_count (box->view);

  (void) pspec;
  gtk_spin_button_set_range (GTK_SPIN_BUTTON (box->number), 1, MAX (top, 1));
  gtk_widget_set_visible (box->number, kind != 2);
  gtk_widget_set_visible (box->names, kind == 2);
}

void
w42_go_to_dialog_show (GtkWindow *parent, W42View *view)
{
  GoToBox *box;
  GtkWidget *content, *grid, *label;

  g_return_if_fail (W42_IS_VIEW (view));

  if (w42_view_get_document (view) == NULL)
    return;

  box = g_new0 (GoToBox, 1);
  box->view = view;
  box->window = dialog_shell (parent, "Go To", &content, view);
  g_object_weak_ref (G_OBJECT (box->window), go_to_free, box);
  box->bookmarks = w42_pt_bookmark_names (w42_document_pt (w42_view_get_document (view)));

  grid = group (content, "Go to What");

  box->what = choice_row (grid, 0, 0, "Go to _What:", GO_TO_KINDS, 0);

  label = gtk_label_new_with_mnemonic ("Enter _Number:");
  box->number = gtk_spin_button_new_with_range (1, 1, 1);
  gtk_spin_button_set_activates_default (GTK_SPIN_BUTTON (box->number), TRUE);
  gtk_label_set_xalign (GTK_LABEL (label), 0.0);
  gtk_label_set_mnemonic_widget (GTK_LABEL (label), box->number);
  gtk_grid_attach (GTK_GRID (grid), label, 0, 1, 1, 1);
  gtk_grid_attach (GTK_GRID (grid), box->number, 1, 1, 1, 1);

  box->names = gtk_drop_down_new_from_strings ((const char * const *) box->bookmarks);
  gtk_widget_set_size_request (box->names, 132, -1);
  gtk_grid_attach (GTK_GRID (grid), box->names, 1, 2, 1, 1);
  gtk_widget_set_visible (box->names, FALSE);

  g_signal_connect (box->what, "notify::selected", G_CALLBACK (on_go_to_kind), box);
  on_go_to_kind (G_OBJECT (box->what), NULL, box);

  button_row (content, box->window, G_CALLBACK (on_go_to_ok), box);
  gtk_window_present (GTK_WINDOW (box->window));
  gtk_widget_grab_focus (box->number);
}

/* ---------------------------------------------------------------------- */
/* Date and Time                                                           */
/* ---------------------------------------------------------------------- */

typedef struct {
  GtkWidget *window;
  W42View   *view;
  GtkWidget *list;      /* GtkListBox of today in each format */
} DateTimeBox;

/* The formats Word 6 offered, near enough, with the ISO one added. */
static const char * const DATE_FORMATS[] = {
  "%d.%m.%Y", "%m/%d/%Y", "%Y-%m-%d", "%d %B %Y", "%B %d, %Y",
  "%A, %d %B %Y", "%A, %B %d, %Y", "%d %b %Y", "%b %d, %Y",
  "%H:%M", "%H:%M:%S", "%I:%M %p", "%d.%m.%Y %H:%M", "%B %d, %Y %H:%M",
};

static void
on_date_time_ok (GtkButton *button, gpointer data)
{
  DateTimeBox *box = data;
  GtkListBoxRow *row = gtk_list_box_get_selected_row (GTK_LIST_BOX (box->list));

  (void) button;

  if (row != NULL)
    {
      GtkWidget *label = gtk_list_box_row_get_child (row);

      if (GTK_IS_LABEL (label))
        w42_view_insert_text (box->view, gtk_label_get_text (GTK_LABEL (label)));
    }

  gtk_window_destroy (GTK_WINDOW (box->window));
}

static void
on_date_time_activated (GtkListBox *list, GtkListBoxRow *row, gpointer data)
{
  (void) list; (void) row;
  on_date_time_ok (NULL, data);
}

void
w42_date_time_dialog_show (GtkWindow *parent, W42View *view)
{
  DateTimeBox *box;
  GtkWidget *content, *grid, *label, *scroller;
  GDateTime *now = g_date_time_new_now_local ();

  g_return_if_fail (W42_IS_VIEW (view));

  if (w42_view_get_document (view) == NULL)
    return;

  box = g_new0 (DateTimeBox, 1);
  box->view = view;
  box->window = dialog_shell (parent, "Date and Time", &content, view);
  g_object_weak_ref (G_OBJECT (box->window), hf_free, box);

  grid = group (content, "Available Formats");

  label = gtk_label_new_with_mnemonic ("_Formats:");
  gtk_label_set_xalign (GTK_LABEL (label), 0.0);
  gtk_grid_attach (GTK_GRID (grid), label, 0, 0, 1, 1);

  box->list = gtk_list_box_new ();
  gtk_list_box_set_selection_mode (GTK_LIST_BOX (box->list), GTK_SELECTION_BROWSE);
  gtk_label_set_mnemonic_widget (GTK_LABEL (label), box->list);
  g_signal_connect (box->list, "row-activated",
                    G_CALLBACK (on_date_time_activated), box);

  for (guint i = 0; i < G_N_ELEMENTS (DATE_FORMATS); i++)
    {
      char *text = g_date_time_format (now, DATE_FORMATS[i]);
      GtkWidget *item = gtk_label_new (text);

      gtk_label_set_xalign (GTK_LABEL (item), 0.0);
      gtk_widget_set_margin_start (item, 4);
      gtk_list_box_append (GTK_LIST_BOX (box->list), item);
      g_free (text);
    }
  gtk_list_box_select_row (GTK_LIST_BOX (box->list),
                           gtk_list_box_get_row_at_index (GTK_LIST_BOX (box->list), 0));

  scroller = gtk_scrolled_window_new ();
  gtk_scrolled_window_set_policy (GTK_SCROLLED_WINDOW (scroller),
                                  GTK_POLICY_NEVER, GTK_POLICY_AUTOMATIC);
  gtk_scrolled_window_set_has_frame (GTK_SCROLLED_WINDOW (scroller), TRUE);
  gtk_scrolled_window_set_child (GTK_SCROLLED_WINDOW (scroller), box->list);
  gtk_widget_set_size_request (scroller, 260, 200);
  gtk_grid_attach (GTK_GRID (grid), scroller, 0, 1, 2, 1);

  g_date_time_unref (now);

  button_row (content, box->window, G_CALLBACK (on_date_time_ok), box);
  gtk_window_present (GTK_WINDOW (box->window));
  gtk_widget_grab_focus (box->list);
}

/* ---------------------------------------------------------------------- */
/* Symbol                                                                  */
/* ---------------------------------------------------------------------- */

/* The characters a keyboard does not have, in the order Word's Symbol box
 * roughly showed them: typography, then currency and maths, then accented
 * Latin, then Greek and arrows. */
static const char *SYMBOLS =
  "\342\200\223\342\200\224\342\200\230\342\200\231\342\200\234\342\200\235\342\200\242\342\200\246"
  "\302\247\302\266\342\200\240\342\200\241\302\260\302\261\303\227\303\267"
  "\302\251\302\256\342\204\242\342\202\254\302\243\302\245\302\242\302\274\302\275\302\276"
  "\302\271\302\262\302\263\302\253\302\273\302\277\302\241\302\265"
  "\303\200\303\201\303\202\303\203\303\204\303\205\303\206\303\207"
  "\303\210\303\211\303\212\303\213\303\214\303\215\303\216\303\217"
  "\303\220\303\221\303\222\303\223\303\224\303\225\303\226\303\230"
  "\303\231\303\232\303\233\303\234\303\235\303\236\303\237"
  "\303\240\303\241\303\242\303\243\303\244\303\245\303\246\303\247"
  "\303\250\303\251\303\252\303\253\303\254\303\255\303\256\303\257"
  "\303\260\303\261\303\262\303\263\303\264\303\265\303\266\303\270"
  "\303\271\303\272\303\273\303\274\303\275\303\276\303\277"
  "\316\261\316\262\316\263\316\264\316\265\316\266\316\267\316\270"
  "\316\271\316\272\316\273\316\274\316\275\316\276\316\277\317\200"
  "\317\201\317\203\317\204\317\205\317\206\317\207\317\210\317\211"
  "\316\224\316\230\316\233\316\236\316\240\316\243\316\246\316\250\316\251"
  "\342\206\220\342\206\222\342\206\221\342\206\223\342\206\224\342\207\222\342\207\224"
  "\342\210\236\342\211\210\342\211\240\342\211\244\342\211\245\342\210\221\342\210\232"
  "\342\210\202\342\210\217\342\210\253\342\231\240\342\231\243\342\231\245\342\231\246"
  "\342\230\272\342\234\223\342\234\227";

static void
on_symbol_activated (GtkFlowBox *flow, GtkFlowBoxChild *child, gpointer data)
{
  W42View *view = data;
  GtkWidget *label = gtk_flow_box_child_get_child (child);

  (void) flow;

  if (GTK_IS_LABEL (label))
    w42_view_insert_text (view, gtk_label_get_text (GTK_LABEL (label)));
}

void
w42_symbol_dialog_show (GtkWindow *parent, W42View *view)
{
  GtkWidget *window, *content, *frame, *flow, *row, *close;
  const char *p;

  g_return_if_fail (W42_IS_VIEW (view));

  if (w42_view_get_document (view) == NULL)
    return;

  window = dialog_shell (parent, "Symbol", &content, view);

  /* Modeless, as Word 6's was: pick a symbol, type, pick another. */
  gtk_window_set_modal (GTK_WINDOW (window), FALSE);

  flow = gtk_flow_box_new ();
  gtk_flow_box_set_selection_mode (GTK_FLOW_BOX (flow), GTK_SELECTION_SINGLE);
  gtk_flow_box_set_min_children_per_line (GTK_FLOW_BOX (flow), 16);
  gtk_flow_box_set_max_children_per_line (GTK_FLOW_BOX (flow), 16);
  gtk_flow_box_set_homogeneous (GTK_FLOW_BOX (flow), TRUE);
  gtk_flow_box_set_activate_on_single_click (GTK_FLOW_BOX (flow), FALSE);
  gtk_flow_box_set_row_spacing (GTK_FLOW_BOX (flow), 1);
  gtk_flow_box_set_column_spacing (GTK_FLOW_BOX (flow), 1);
  gtk_widget_add_css_class (flow, "w42-symbols");
  g_signal_connect (flow, "child-activated", G_CALLBACK (on_symbol_activated), view);

  for (p = SYMBOLS; *p != '\0'; p = g_utf8_next_char (p))
    {
      char one[8] = { 0 };
      GtkWidget *label;

      g_unichar_to_utf8 (g_utf8_get_char (p), one);
      label = gtk_label_new (one);
      gtk_widget_set_size_request (label, 26, 26);
      gtk_flow_box_append (GTK_FLOW_BOX (flow), label);
    }

  frame = gtk_frame_new (NULL);
  gtk_frame_set_child (GTK_FRAME (frame), flow);
  gtk_box_append (GTK_BOX (content), frame);

  row = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 8);
  gtk_widget_set_halign (row, GTK_ALIGN_END);
  close = gtk_button_new_with_mnemonic ("Close");
  gtk_widget_set_size_request (close, 92, 26);
  g_signal_connect_swapped (close, "clicked", G_CALLBACK (gtk_window_destroy), window);
  gtk_box_append (GTK_BOX (row), close);
  gtk_box_append (GTK_BOX (content), row);

  gtk_window_present (GTK_WINDOW (window));
}

/* ---------------------------------------------------------------------- */
/* AutoCorrect                                                             */
/* ---------------------------------------------------------------------- */

typedef struct {
  GtkWidget *window;
  W42View   *view;
  GtkWidget *on;
} AutoBox;

static void
on_autocorrect_ok (GtkButton *button, gpointer data)
{
  AutoBox *box = data;
  gboolean on = gtk_check_button_get_active (GTK_CHECK_BUTTON (box->on));

  (void) button;
  w42_settings_set_bool ("auto-correct", on);
  w42_view_set_autocorrect (box->view, on);
  gtk_window_destroy (GTK_WINDOW (box->window));
}

void
w42_autocorrect_dialog_show (GtkWindow *parent, W42View *view)
{
  AutoBox *box;
  GtkWidget *content, *grid, *label, *list, *scroller;
  const char *const *pairs = w42_autocorrect_replacements ();

  g_return_if_fail (W42_IS_VIEW (view));

  box = g_new0 (AutoBox, 1);
  box->view = view;
  box->window = dialog_shell (parent, "AutoCorrect", &content, view);
  g_object_weak_ref (G_OBJECT (box->window), hf_free, box);

  grid = group (content, "As You Type");
  box->on = gtk_check_button_new_with_mnemonic ("_Correct as you type");
  gtk_check_button_set_active (GTK_CHECK_BUTTON (box->on),
                               w42_view_get_autocorrect (view));
  gtk_grid_attach (GTK_GRID (grid), box->on, 0, 0, 2, 1);

  label = gtk_label_new ("Straight quotes become curly ones, two capitals at "
                         "the start of a word become one, a sentence takes a "
                         "capital, two hyphens become a dash, and the words "
                         "below are put right.");
  gtk_label_set_wrap (GTK_LABEL (label), TRUE);
  gtk_label_set_max_width_chars (GTK_LABEL (label), 48);
  gtk_label_set_xalign (GTK_LABEL (label), 0.0);
  gtk_widget_add_css_class (label, "w42-dialog-status");
  gtk_grid_attach (GTK_GRID (grid), label, 0, 1, 2, 1);

  grid = group (content, "Replace");
  list = gtk_list_box_new ();
  gtk_list_box_set_selection_mode (GTK_LIST_BOX (list), GTK_SELECTION_NONE);
  for (guint i = 0; pairs[i] != NULL; i += 2)
    {
      char *text = g_strdup_printf ("%-12s  %s", pairs[i], pairs[i + 1]);
      GtkWidget *row = gtk_label_new (text);

      gtk_label_set_xalign (GTK_LABEL (row), 0.0);
      gtk_widget_set_margin_start (row, 4);
      gtk_list_box_append (GTK_LIST_BOX (list), row);
      g_free (text);
    }
  scroller = gtk_scrolled_window_new ();
  gtk_scrolled_window_set_policy (GTK_SCROLLED_WINDOW (scroller),
                                  GTK_POLICY_NEVER, GTK_POLICY_AUTOMATIC);
  gtk_scrolled_window_set_has_frame (GTK_SCROLLED_WINDOW (scroller), TRUE);
  gtk_scrolled_window_set_child (GTK_SCROLLED_WINDOW (scroller), list);
  gtk_widget_set_size_request (scroller, 260, 150);
  gtk_grid_attach (GTK_GRID (grid), scroller, 0, 0, 2, 1);

  button_row (content, box->window, G_CALLBACK (on_autocorrect_ok), box);
  gtk_window_present (GTK_WINDOW (box->window));
}

/* ---------------------------------------------------------------------- */
/* Tabs                                                                    */
/* ---------------------------------------------------------------------- */

typedef struct {
  GtkWidget  *window;
  W42View    *view;
  GtkWidget  *position;    /* inches */
  GtkWidget  *alignment;
  GtkWidget  *leader;      /* what fills the gap in front of the stop */
  GtkWidget  *list;        /* GtkListBox of the stops */
  W42ParaFmt  pa;          /* the stops as edited; applied by OK */
} TabsBox;

static const char * const TAB_KINDS[] = { "Left", "Center", "Right", "Decimal", NULL };
/* Word 6's four, in its order and drawn as it drew them. */
static const char * const TAB_LEADERS[] = { "1  None", "2  ......", "3  ------", "4  ______", NULL };

static void
tabs_refresh (TabsBox *box)
{
  GtkWidget *row;

  while ((row = gtk_widget_get_first_child (box->list)) != NULL)
    gtk_list_box_remove (GTK_LIST_BOX (box->list), row);

  for (int i = 0; i < box->pa.n_tabs; i++)
    {
      static const char * const LEADS[] = { "", "  ......", "  ------", "  ______" };
      W42TabKind kind = W42_TAB_KIND (box->pa.tab_kind[i]);
      W42TabLeader lead = W42_TAB_LEADER (box->pa.tab_kind[i]);
      char *text = g_strdup_printf ("%.2f%s  %s%s", measure_from_twips (box->pa.tab_pos[i]),
                                    w42_settings_unit_name (),
                                    TAB_KINDS[CLAMP ((int) kind, 0, 3)],
                                    LEADS[CLAMP ((int) lead, 0, 3)]);
      GtkWidget *label = gtk_label_new (text);

      gtk_label_set_xalign (GTK_LABEL (label), 0.0);
      gtk_widget_set_margin_start (label, 4);
      gtk_list_box_append (GTK_LIST_BOX (box->list), label);
      g_free (text);
    }
}

static int
tabs_entry_twips (TabsBox *box)
{
  return (int) lround (twips_from_measure (gtk_spin_button_get_value (GTK_SPIN_BUTTON (box->position))));
}

static void
on_tabs_set (GtkButton *button, gpointer data)
{
  TabsBox *box = data;

  (void) button;
  w42_para_fmt_set_tab_leader (&box->pa, tabs_entry_twips (box),
                               (W42TabKind) gtk_drop_down_get_selected (GTK_DROP_DOWN (box->alignment)),
                               (W42TabLeader) gtk_drop_down_get_selected (GTK_DROP_DOWN (box->leader)));
  tabs_refresh (box);
}

static void
on_tabs_clear (GtkButton *button, gpointer data)
{
  TabsBox *box = data;

  (void) button;
  w42_para_fmt_clear_tab (&box->pa, tabs_entry_twips (box));
  tabs_refresh (box);
}

static void
on_tabs_clear_all (GtkButton *button, gpointer data)
{
  TabsBox *box = data;

  (void) button;
  box->pa.n_tabs = 0;
  memset (box->pa.tab_pos, 0, sizeof box->pa.tab_pos);
  memset (box->pa.tab_kind, 0, sizeof box->pa.tab_kind);
  tabs_refresh (box);
}

static void
on_tabs_row_selected (GtkListBox *list, GtkListBoxRow *row, gpointer data)
{
  TabsBox *box = data;
  int i;

  (void) list;

  if (row == NULL)
    return;
  i = gtk_list_box_row_get_index (row);
  if (i < 0 || i >= box->pa.n_tabs)
    return;

  gtk_spin_button_set_value (GTK_SPIN_BUTTON (box->position),
                             measure_from_twips (box->pa.tab_pos[i]));
  gtk_drop_down_set_selected (GTK_DROP_DOWN (box->alignment),
                              W42_TAB_KIND (box->pa.tab_kind[i]));
  gtk_drop_down_set_selected (GTK_DROP_DOWN (box->leader),
                              W42_TAB_LEADER (box->pa.tab_kind[i]));
}

static void
on_tabs_ok (GtkButton *button, gpointer data)
{
  TabsBox *box = data;

  (void) button;
  w42_view_apply_para_fmt (box->view, W42_PARA_TABS, &box->pa);
  gtk_window_destroy (GTK_WINDOW (box->window));
}

void
w42_tabs_dialog_show (GtkWindow *parent, W42View *view)
{
  TabsBox *box;
  GtkWidget *content, *grid, *label, *scroller, *buttons, *set, *clear, *clear_all;

  g_return_if_fail (W42_IS_VIEW (view));

  if (w42_view_get_document (view) == NULL)
    return;

  box = g_new0 (TabsBox, 1);
  box->view = view;
  box->window = dialog_shell (parent, "Tabs", &content, view);
  g_object_weak_ref (G_OBJECT (box->window), hf_free, box);
  w42_view_get_para_fmt (view, &box->pa);

  grid = group (content, "Tab Stops");

  label = gtk_label_new_with_mnemonic ("_Tab Stop Position:");
  box->position = gtk_spin_button_new_with_range (0.0, 56.0, 0.05);
  gtk_spin_button_set_digits (GTK_SPIN_BUTTON (box->position), 2);
  gtk_spin_button_set_value (GTK_SPIN_BUTTON (box->position), measure_from_twips (720));
  gtk_widget_set_size_request (box->position, 84, -1);
  gtk_label_set_xalign (GTK_LABEL (label), 0.0);
  gtk_label_set_mnemonic_widget (GTK_LABEL (label), box->position);
  gtk_grid_attach (GTK_GRID (grid), label, 0, 0, 1, 1);
  gtk_grid_attach (GTK_GRID (grid), box->position, 1, 0, 1, 1);

  box->alignment = choice_row (grid, 1, 0, "_Alignment:", TAB_KINDS, 0);
  box->leader = choice_row (grid, 2, 0, "_Leader:", TAB_LEADERS, 0);

  box->list = gtk_list_box_new ();
  gtk_list_box_set_selection_mode (GTK_LIST_BOX (box->list), GTK_SELECTION_SINGLE);
  g_signal_connect (box->list, "row-selected", G_CALLBACK (on_tabs_row_selected), box);
  scroller = gtk_scrolled_window_new ();
  gtk_scrolled_window_set_policy (GTK_SCROLLED_WINDOW (scroller),
                                  GTK_POLICY_NEVER, GTK_POLICY_AUTOMATIC);
  gtk_scrolled_window_set_has_frame (GTK_SCROLLED_WINDOW (scroller), TRUE);
  gtk_scrolled_window_set_child (GTK_SCROLLED_WINDOW (scroller), box->list);
  gtk_widget_set_size_request (scroller, 220, 120);
  gtk_grid_attach (GTK_GRID (grid), scroller, 0, 3, 2, 1);

  buttons = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 8);
  set = gtk_button_new_with_mnemonic ("_Set");
  clear = gtk_button_new_with_mnemonic ("Cl_ear");
  clear_all = gtk_button_new_with_mnemonic ("Clear A_ll");
  g_signal_connect (set, "clicked", G_CALLBACK (on_tabs_set), box);
  g_signal_connect (clear, "clicked", G_CALLBACK (on_tabs_clear), box);
  g_signal_connect (clear_all, "clicked", G_CALLBACK (on_tabs_clear_all), box);
  gtk_box_append (GTK_BOX (buttons), set);
  gtk_box_append (GTK_BOX (buttons), clear);
  gtk_box_append (GTK_BOX (buttons), clear_all);
  gtk_grid_attach (GTK_GRID (grid), buttons, 0, 4, 2, 1);

  tabs_refresh (box);

  button_row (content, box->window, G_CALLBACK (on_tabs_ok), box);
  gtk_window_present (GTK_WINDOW (box->window));
  gtk_widget_grab_focus (box->position);
}

/* ---------------------------------------------------------------------- */
/* Options                                                                 */
/* ---------------------------------------------------------------------- */

typedef struct {
  GtkWidget *window;
  GtkWindow *parent;
  W42View   *view;
  GtkWidget *units;
  GtkWidget *default_view;
  GtkWidget *zoom;
  GtkWidget *auto_spell;
  GtkWidget *auto_correct;
  GtkWidget *user_name;
} OptionsBox;

static const char * const UNIT_NAMES[] = { "Inches", "Centimeters", NULL };
static const char * const VIEW_NAMES[] = { "Normal", "Page Layout", NULL };
static const char * const ZOOM_NAMES[] = { "75%", "100%", "150%", "200%", NULL };
static const int ZOOM_VALUES[] = { 75, 100, 150, 200 };

static void
on_options_ok (GtkButton *button, gpointer data)
{
  OptionsBox *box = data;
  guint zoom = gtk_drop_down_get_selected (GTK_DROP_DOWN (box->zoom));
  gboolean want_spell = gtk_check_button_get_active (GTK_CHECK_BUTTON (box->auto_spell));
  GAction *spell_action;

  (void) button;

  w42_settings_set_units (gtk_drop_down_get_selected (GTK_DROP_DOWN (box->units)) == 1
                            ? W42_UNITS_CM : W42_UNITS_INCHES);
  w42_settings_set_string ("default-view",
                           gtk_drop_down_get_selected (GTK_DROP_DOWN (box->default_view)) == 1
                             ? "page-layout" : "normal");
  if (zoom < G_N_ELEMENTS (ZOOM_VALUES))
    w42_settings_set_int ("zoom", ZOOM_VALUES[zoom]);
  w42_settings_set_bool ("auto-spell", want_spell);
  {
    gboolean want_correct = gtk_check_button_get_active (GTK_CHECK_BUTTON (box->auto_correct));

    w42_settings_set_bool ("auto-correct", want_correct);
    w42_view_set_autocorrect (box->view, want_correct);
  }
  /* The view and zoom chosen apply to this window now, not only to the
   * next one opened. */
  {
    guint dv = gtk_drop_down_get_selected (GTK_DROP_DOWN (box->default_view));

    g_action_group_activate_action (G_ACTION_GROUP (box->parent), "view-mode",
                                    g_variant_new_string (dv == 1 ? "page-layout" : "normal"));
    if (zoom < G_N_ELEMENTS (ZOOM_VALUES))
      g_action_group_activate_action (G_ACTION_GROUP (box->parent), "zoom",
                                      g_variant_new_double (ZOOM_VALUES[zoom] / 100.0));
  }
  {
    const char *name = gtk_editable_get_text (GTK_EDITABLE (box->user_name));

    w42_settings_set_string ("user-name", name);
    w42_pt_set_author (w42_document_pt (w42_view_get_document (box->view)), name);
  }

  /* Spelling as you type takes effect now, through the same action the
   * Tools menu toggles. */
  spell_action = g_action_map_lookup_action (G_ACTION_MAP (box->parent), "auto-spell");
  if (spell_action != NULL && g_action_get_enabled (spell_action))
    {
      GVariant *state = g_action_get_state (spell_action);
      gboolean on = g_variant_get_boolean (state);

      g_variant_unref (state);
      if (on != want_spell)
        g_action_activate (spell_action, NULL);
    }

  /* The ruler draws in the new unit. */
  gtk_widget_queue_draw (GTK_WIDGET (box->parent));

  gtk_window_destroy (GTK_WINDOW (box->window));
}

void
w42_options_dialog_show (GtkWindow *parent, W42View *view)
{
  OptionsBox *box;
  GtkWidget *content, *grid;
  char *default_view;
  int zoom;
  guint zoom_index = 1;

  g_return_if_fail (W42_IS_VIEW (view));

  box = g_new0 (OptionsBox, 1);
  box->parent = parent;
  box->view = view;
  box->window = dialog_shell (parent, "Options", &content, view);
  g_object_weak_ref (G_OBJECT (box->window), hf_free, box);

  grid = group (content, "General");
  box->units = choice_row (grid, 0, 0, "_Measurement Units:", UNIT_NAMES,
                           w42_settings_get_units () == W42_UNITS_CM ? 1 : 0);

  grid = group (content, "View");
  default_view = w42_settings_get_string ("default-view", "page-layout");
  box->default_view = choice_row (grid, 0, 0, "Default _View:", VIEW_NAMES,
                                  g_str_equal (default_view, "page-layout") ? 1 : 0);
  g_free (default_view);

  zoom = w42_settings_get_int ("zoom", 100);
  for (guint i = 0; i < G_N_ELEMENTS (ZOOM_VALUES); i++)
    if (ZOOM_VALUES[i] == zoom)
      zoom_index = i;
  box->zoom = choice_row (grid, 1, 0, "Default _Zoom:", ZOOM_NAMES, zoom_index);

  grid = group (content, "User Info");
  {
    GtkWidget *label = gtk_label_new_with_mnemonic ("_Name:");
    char *name = w42_settings_get_string ("user-name", "");

    box->user_name = gtk_entry_new ();
    if (*name == '\0')
      {
        const char *real = g_get_real_name ();

        gtk_editable_set_text (GTK_EDITABLE (box->user_name),
                               real != NULL && !g_str_equal (real, "Unknown") ? real : g_get_user_name ());
      }
    else
      gtk_editable_set_text (GTK_EDITABLE (box->user_name), name);
    g_free (name);
    gtk_widget_set_tooltip_text (box->user_name, "Annotations and revisions carry this name in the files you save.");
    gtk_label_set_xalign (GTK_LABEL (label), 0.0);
    gtk_label_set_mnemonic_widget (GTK_LABEL (label), box->user_name);
    gtk_widget_set_size_request (box->user_name, 200, -1);
    gtk_grid_attach (GTK_GRID (grid), label, 0, 0, 1, 1);
    gtk_grid_attach (GTK_GRID (grid), box->user_name, 1, 0, 1, 1);
  }

  grid = group (content, "Spelling");
  box->auto_correct = gtk_check_button_new_with_mnemonic ("Correct as you t_ype");
  gtk_check_button_set_active (GTK_CHECK_BUTTON (box->auto_correct),
                               w42_settings_get_bool ("auto-correct", TRUE));
  gtk_grid_attach (GTK_GRID (grid), box->auto_correct, 0, 1, 2, 1);

  box->auto_spell = gtk_check_button_new_with_mnemonic ("_Check spelling as you type");
  gtk_check_button_set_active (GTK_CHECK_BUTTON (box->auto_spell),
                               w42_settings_get_bool ("auto-spell", TRUE));
  gtk_grid_attach (GTK_GRID (grid), box->auto_spell, 0, 0, 2, 1);

  button_row (content, box->window, G_CALLBACK (on_options_ok), box);
  gtk_window_present (GTK_WINDOW (box->window));
  gtk_widget_grab_focus (box->units);
}

/* ---------------------------------------------------------------------- */
/* Borders and Shading                                                     */
/* ---------------------------------------------------------------------- */

typedef struct {
  GtkWidget *window;
  W42View   *view;
  GtkWidget *sides[4];     /* top, bottom, left, right */
  GtkWidget *width;
  GtkWidget *line_colour;
  GtkWidget *style;
  GtkWidget *inside;       /* a table's rules between its cells */
  GtkWidget *apply_to;     /* NULL outside a table */
  GtkWidget *shading;
  GtkWidget *fill;
} BordersBox;

/* The sixteen colours Word 6 offered, which is what a colour is chosen from
 * anywhere in Word42. */
static const char *const PALETTE_NAMES[] = {
  "Black", "Blue", "Cyan", "Green", "Magenta", "Red", "Yellow", "White",
  "Dark Blue", "Dark Cyan", "Dark Green", "Dark Magenta", "Dark Red",
  "Dark Yellow", "Dark Gray", "Light Gray", NULL
};
static const guint32 PALETTE_VALUES[] = {
  0x000000, 0x0000FF, 0x00FFFF, 0x00FF00, 0xFF00FF, 0xFF0000, 0xFFFF00, 0xFFFFFF,
  0x000080, 0x008080, 0x008000, 0x800080, 0x800000, 0x808000, 0x808080, 0xC0C0C0
};
/* The same list with "None" in front, for a background that may have none. */
static const char *const FILL_NAMES[] = {
  "None",
  "Black", "Blue", "Cyan", "Green", "Magenta", "Red", "Yellow", "White",
  "Dark Blue", "Dark Cyan", "Dark Green", "Dark Magenta", "Dark Red",
  "Dark Yellow", "Dark Gray", "Light Gray", NULL
};

/* Which entry of the palette a colour is, or 0 for one that is not in it. */
static guint
palette_index (guint32 rgb)
{
  for (guint i = 0; i < G_N_ELEMENTS (PALETTE_VALUES); i++)
    if (PALETTE_VALUES[i] == (rgb & 0xFFFFFF))
      return i;
  return 0;
}

/* Word XP's line widths, from a quarter point to six. */
static const char * const BORDER_WIDTHS[] = {
  "\302\274 pt", "\302\275 pt", "\302\276 pt", "1 pt", "1\302\275 pt", "2\302\274 pt",
  "3 pt", "4\302\275 pt", "6 pt", NULL
};
static const int BORDER_WIDTH_TWIPS[] = { 5, 10, 15, 20, 30, 45, 60, 90, 120 };
static const char * const BORDER_STYLES[] = { "Single", "Double", "Dashed", "Dotted", NULL };
static const char * const APPLY_TO[] = { "Paragraph", "Cell", "Table", NULL };

/* The entry of the width list nearest `twips`. */
static guint
width_index_for (int twips)
{
  guint best = 0;

  for (guint i = 0; i < G_N_ELEMENTS (BORDER_WIDTH_TWIPS); i++)
    if (ABS (BORDER_WIDTH_TWIPS[i] - twips) < ABS (BORDER_WIDTH_TWIPS[best] - twips))
      best = i;
  return best;
}
static const char * const SHADINGS[] = {
  "None", "10%", "20%", "30%", "40%", "50%", "75%", "Solid (100%)", NULL
};
static const int SHADING_VALUES[] = { 0, 10, 20, 30, 40, 50, 75, 100 };

static void
on_borders_ok (GtkButton *button, gpointer data)
{
  BordersBox *box = data;
  W42ParaFmt want;
  guint w = gtk_drop_down_get_selected (GTK_DROP_DOWN (box->width));
  guint sh = gtk_drop_down_get_selected (GTK_DROP_DOWN (box->shading));
  guint lc = gtk_drop_down_get_selected (GTK_DROP_DOWN (box->line_colour));
  guint bg = gtk_drop_down_get_selected (GTK_DROP_DOWN (box->fill));
  static const guint8 bits[4] = { W42_BORDER_TOP, W42_BORDER_BOTTOM,
                                  W42_BORDER_LEFT, W42_BORDER_RIGHT };

  (void) button;

  guint st = gtk_drop_down_get_selected (GTK_DROP_DOWN (box->style));
  guint target = box->apply_to != NULL ? gtk_drop_down_get_selected (GTK_DROP_DOWN (box->apply_to)) : 0;
  W42BorderEdge line;

  memset (&want, 0, sizeof want);
  for (int i = 0; i < 4; i++)
    if (gtk_check_button_get_active (GTK_CHECK_BUTTON (box->sides[i])))
      want.border |= bits[i];
  line.width = (guint8) BORDER_WIDTH_TWIPS[MIN (w, G_N_ELEMENTS (BORDER_WIDTH_TWIPS) - 1)];
  line.color = PALETTE_VALUES[MIN (lc, G_N_ELEMENTS (PALETTE_VALUES) - 1)];
  line.style = (guint8) MIN (st, W42_BORDER_DOTTED);
  w42_para_fmt_set_edges (&want, line.width, line.color, (W42BorderStyle) line.style);
  /* A colour behind the paragraph is a colour; without one it is a
   * percentage of black, as Word 6 had it. */
  if (bg > 0)
    {
      want.has_shading_color = 1;
      want.shading_color = PALETTE_VALUES[MIN (bg - 1, G_N_ELEMENTS (PALETTE_VALUES) - 1)];
    }
  else
    want.shading = (guint8) SHADING_VALUES[MIN (sh, 7)];

  if (target == 1)
    {
      /* The cell: its mark takes the sides, their line and the shading. */
      W42ParaFmt cell;

      if (w42_view_cell_get_fmt (box->view, &cell))
        {
          cell.border = (guint8) (W42_BORDER_CELL_SET | want.border);
          memcpy (cell.edge, want.edge, sizeof cell.edge);
          cell.has_shading_color = want.has_shading_color;
          cell.shading_color = want.shading_color;
          cell.shading = want.shading;
          w42_view_cell_set_fmt (box->view, &cell);
        }
    }
  else if (target == 2)
    {
      /* The table: the checked sides are its outside, the line inside
       * is the rule between its cells. */
      W42BorderEdge outer[4], inside;
      gboolean any = want.border != 0;

      for (int i = 0; i < 4; i++)
        {
          outer[i] = line;
          if (!(want.border & (1 << i)))
            outer[i].style = W42_BORDER_NONE;
        }
      inside = line;
      if (!gtk_check_button_get_active (GTK_CHECK_BUTTON (box->inside)))
        inside.style = W42_BORDER_NONE;
      else
        any = TRUE;
      w42_view_table_set_borders (box->view, any);
      w42_view_table_set_edges (box->view, outer, &inside);
      {
        /* The table's own rules, so the cells no longer say otherwise. */
        W42ParaFmt cell;

        if (w42_view_cell_get_fmt (box->view, &cell))
          {
            cell.border = 0;
            memset (cell.edge, 0, sizeof cell.edge);
            w42_view_cell_set_fmt (box->view, &cell);
          }
      }
      w42_view_apply_para_fmt (box->view, W42_PARA_SHADING, &want);
    }
  else
    w42_view_apply_para_fmt (box->view, W42_PARA_BORDER | W42_PARA_SHADING, &want);
  gtk_window_destroy (GTK_WINDOW (box->window));
}

/* The table's four sides and its inside rule, as set from the table's
 * edges when the dialog is asked to apply to the table. */
static void
on_borders_apply_to (GObject *dropdown, GParamSpec *pspec, gpointer data)
{
  BordersBox *box = data;
  guint target = gtk_drop_down_get_selected (GTK_DROP_DOWN (dropdown));
  W42ParaFmt fmt;
  W42BorderEdge edges[W42_N_EDGES];
  static const guint8 bits[4] = { W42_BORDER_TOP, W42_BORDER_BOTTOM, W42_BORDER_LEFT, W42_BORDER_RIGHT };
  const W42BorderEdge *lead = NULL;

  (void) pspec;
  if (target == 0)
    {
      w42_view_get_para_fmt (box->view, &fmt);
      for (int i = 0; i < 4; i++)
        gtk_check_button_set_active (GTK_CHECK_BUTTON (box->sides[i]), (fmt.border & bits[i]) != 0);
      lead = &fmt.edge[0];
    }
  else if (target == 1)
    {
      int sides = w42_view_cell_get_borders (box->view);

      if (w42_view_cell_get_fmt (box->view, &fmt))
        {
          for (int i = 0; i < 4; i++)
            gtk_check_button_set_active (GTK_CHECK_BUTTON (box->sides[i]), (sides & bits[i]) != 0);
          lead = &fmt.edge[0];
        }
    }
  else if (w42_view_table_get_edges (box->view, edges))
    {
      gboolean ruled = w42_view_table_get_borders (box->view);

      for (int i = 0; i < 4; i++)
        gtk_check_button_set_active (GTK_CHECK_BUTTON (box->sides[i]),
                                     ruled && edges[i].style != W42_BORDER_NONE);
      gtk_check_button_set_active (GTK_CHECK_BUTTON (box->inside),
                                   ruled && edges[W42_EDGE_INSIDE_H].style != W42_BORDER_NONE);
      lead = &edges[0];
    }
  gtk_widget_set_sensitive (box->inside, target == 2);
  if (lead != NULL)
    {
      gtk_drop_down_set_selected (GTK_DROP_DOWN (box->width), width_index_for (W42_EDGE_WIDTH (lead)));
      gtk_drop_down_set_selected (GTK_DROP_DOWN (box->line_colour), palette_index (lead->color));
      gtk_drop_down_set_selected (GTK_DROP_DOWN (box->style),
                                  lead->style <= W42_BORDER_DOTTED ? lead->style : 0);
    }
}

static void
on_borders_preset (GtkButton *button, gpointer data)
{
  BordersBox *box = data;
  gboolean all = g_str_equal (gtk_button_get_label (button), "_Box");

  for (int i = 0; i < 4; i++)
    gtk_check_button_set_active (GTK_CHECK_BUTTON (box->sides[i]), all);
}

void
w42_borders_dialog_show (GtkWindow *parent, W42View *view)
{
  BordersBox *box;
  GtkWidget *content, *grid, *presets, *none, *all;
  W42ParaFmt now;
  static const char * const side_labels[4] = { "_Top", "Botto_m", "_Left", "_Right" };
  static const guint8 bits[4] = { W42_BORDER_TOP, W42_BORDER_BOTTOM,
                                  W42_BORDER_LEFT, W42_BORDER_RIGHT };
  guint width_index = 0, shading_index = 0;

  g_return_if_fail (W42_IS_VIEW (view));

  if (w42_view_get_document (view) == NULL)
    return;

  box = g_new0 (BordersBox, 1);
  box->view = view;
  box->window = dialog_shell (parent, "Borders and Shading", &content, view);
  g_object_weak_ref (G_OBJECT (box->window), hf_free, box);
  w42_view_get_para_fmt (view, &now);

  grid = group (content, "Borders");

  presets = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 8);
  none = gtk_button_new_with_mnemonic ("_None");
  all = gtk_button_new_with_mnemonic ("_Box");
  g_signal_connect (none, "clicked", G_CALLBACK (on_borders_preset), box);
  g_signal_connect (all, "clicked", G_CALLBACK (on_borders_preset), box);
  gtk_box_append (GTK_BOX (presets), none);
  gtk_box_append (GTK_BOX (presets), all);
  gtk_grid_attach (GTK_GRID (grid), presets, 0, 0, 2, 1);

  for (int i = 0; i < 4; i++)
    {
      box->sides[i] = gtk_check_button_new_with_mnemonic (side_labels[i]);
      gtk_check_button_set_active (GTK_CHECK_BUTTON (box->sides[i]),
                                   (now.border & bits[i]) != 0);
      gtk_grid_attach (GTK_GRID (grid), box->sides[i], i % 2, 1 + i / 2, 1, 1);
    }
  box->inside = gtk_check_button_new_with_mnemonic ("_Inside (between the cells)");
  gtk_widget_set_sensitive (box->inside, FALSE);
  gtk_grid_attach (GTK_GRID (grid), box->inside, 0, 3, 2, 1);

  width_index = width_index_for (w42_para_fmt_border_width (&now));
  box->style = choice_row (grid, 4, 0, "St_yle:", BORDER_STYLES,
                           MIN (w42_para_fmt_border_style (&now), W42_BORDER_DOTTED));
  box->width = choice_row (grid, 5, 0, "Line _Width:", BORDER_WIDTHS, width_index);
  box->line_colour = choice_row (grid, 6, 0, "Line _Color:", PALETTE_NAMES,
                                 palette_index (w42_para_fmt_border_color (&now)));
  if (w42_view_in_table (view))
    {
      /* In a table, Word XP's dialog could rule the cell or the whole
       * table as well as the paragraph. */
      box->apply_to = choice_row (grid, 7, 0, "_Apply to:", APPLY_TO, 1);
      g_signal_connect (box->apply_to, "notify::selected", G_CALLBACK (on_borders_apply_to), box);
      on_borders_apply_to (G_OBJECT (box->apply_to), NULL, box);
    }

  grid = group (content, "Shading");
  for (guint i = 0; i < G_N_ELEMENTS (SHADING_VALUES); i++)
    if (SHADING_VALUES[i] == now.shading)
      shading_index = i;
  box->shading = choice_row (grid, 0, 0, "_Shading:", SHADINGS, shading_index);
  box->fill = choice_row (grid, 1, 0, "Bac_kground:", FILL_NAMES,
                          now.has_shading_color
                            ? palette_index (now.shading_color) + 1 : 0);

  button_row (content, box->window, G_CALLBACK (on_borders_ok), box);
  gtk_window_present (GTK_WINDOW (box->window));
  gtk_widget_grab_focus (box->sides[0]);
}

/* ---------------------------------------------------------------------- */
/* Hyperlink                                                               */
/* ---------------------------------------------------------------------- */

typedef struct {
  GtkWidget *window;
  W42View   *view;
  GtkWidget *address;
  GtkWidget *text;
} LinkBox;

static void
on_hyperlink_ok (GtkButton *button, gpointer data)
{
  LinkBox *box = data;
  const char *url = gtk_editable_get_text (GTK_EDITABLE (box->address));
  const char *text = gtk_editable_get_text (GTK_EDITABLE (box->text));

  (void) button;
  w42_view_set_link (box->view, url, text);
  gtk_window_destroy (GTK_WINDOW (box->window));
}

static void
on_hyperlink_remove (GtkButton *button, gpointer data)
{
  LinkBox *box = data;

  (void) button;
  w42_view_set_link (box->view, NULL, NULL);
  gtk_window_destroy (GTK_WINDOW (box->window));
}

void
w42_hyperlink_dialog_show (GtkWindow *parent, W42View *view)
{
  LinkBox *box;
  GtkWidget *content, *grid, *label, *remove;
  const char *current;
  char *selected;

  g_return_if_fail (W42_IS_VIEW (view));

  if (w42_view_get_document (view) == NULL)
    return;

  box = g_new0 (LinkBox, 1);
  box->view = view;
  box->window = dialog_shell (parent, "Hyperlink", &content, view);
  g_object_weak_ref (G_OBJECT (box->window), hf_free, box);

  grid = group (content, "Link");

  label = gtk_label_new_with_mnemonic ("_Address:");
  box->address = gtk_entry_new ();
  gtk_widget_set_size_request (box->address, 300, -1);
  gtk_entry_set_activates_default (GTK_ENTRY (box->address), TRUE);
  gtk_label_set_xalign (GTK_LABEL (label), 0.0);
  gtk_label_set_mnemonic_widget (GTK_LABEL (label), box->address);
  gtk_grid_attach (GTK_GRID (grid), label, 0, 0, 1, 1);
  gtk_grid_attach (GTK_GRID (grid), box->address, 1, 0, 1, 1);

  label = gtk_label_new_with_mnemonic ("_Text to Display:");
  box->text = gtk_entry_new ();
  gtk_entry_set_activates_default (GTK_ENTRY (box->text), TRUE);
  gtk_label_set_xalign (GTK_LABEL (label), 0.0);
  gtk_label_set_mnemonic_widget (GTK_LABEL (label), box->text);
  gtk_grid_attach (GTK_GRID (grid), label, 0, 1, 1, 1);
  gtk_grid_attach (GTK_GRID (grid), box->text, 1, 1, 1, 1);

  current = w42_view_get_link (view);
  if (current != NULL)
    gtk_editable_set_text (GTK_EDITABLE (box->address), current);
  selected = w42_view_get_selected_text (view);
  if (selected != NULL && *selected != '\0')
    {
      gtk_editable_set_text (GTK_EDITABLE (box->text), selected);
      gtk_widget_set_sensitive (box->text, FALSE);   /* the selection is the text */
    }
  g_free (selected);

  remove = gtk_button_new_with_mnemonic ("_Remove Link");
  gtk_widget_set_sensitive (remove, current != NULL);
  g_signal_connect (remove, "clicked", G_CALLBACK (on_hyperlink_remove), box);
  gtk_grid_attach (GTK_GRID (grid), remove, 1, 2, 1, 1);

  button_row (content, box->window, G_CALLBACK (on_hyperlink_ok), box);
  gtk_window_present (GTK_WINDOW (box->window));
  gtk_widget_grab_focus (box->address);
}

/* ---------------------------------------------------------------------- */
/* Bookmark                                                                */
/* ---------------------------------------------------------------------- */

typedef struct {
  GtkWidget *window;
  W42View   *view;
  GtkWidget *name;
  GtkWidget *list;      /* GtkListBox of names */
  GtkWidget *status;
  char     **names;
} BookmarkBox;

static void
bookmark_free (gpointer data, GObject *gone)
{
  BookmarkBox *box = data;

  (void) gone;
  g_strfreev (box->names);
  g_free (box);
}

static void
bookmark_refresh (BookmarkBox *box)
{
  GtkWidget *row;

  while ((row = gtk_widget_get_first_child (box->list)) != NULL)
    gtk_list_box_remove (GTK_LIST_BOX (box->list), row);

  g_strfreev (box->names);
  box->names = w42_pt_bookmark_names (w42_document_pt (w42_view_get_document (box->view)));
  for (guint i = 0; box->names != NULL && box->names[i] != NULL; i++)
    {
      GtkWidget *label = gtk_label_new (box->names[i]);

      gtk_label_set_xalign (GTK_LABEL (label), 0.0);
      gtk_widget_set_margin_start (label, 4);
      gtk_list_box_append (GTK_LIST_BOX (box->list), label);
    }
}

static void
on_bookmark_add (GtkButton *button, gpointer data)
{
  BookmarkBox *box = data;
  const char *name = gtk_editable_get_text (GTK_EDITABLE (box->name));

  (void) button;

  if (name == NULL || *name == '\0')
    {
      gtk_label_set_text (GTK_LABEL (box->status), "Type a name for the bookmark.");
      return;
    }
  if (!w42_view_has_selection (box->view))
    {
      gtk_label_set_text (GTK_LABEL (box->status), "Select the text to bookmark first.");
      return;
    }

  w42_view_set_bookmark (box->view, name);
  gtk_label_set_text (GTK_LABEL (box->status), "");
  bookmark_refresh (box);
}

static void
on_bookmark_go_to (GtkButton *button, gpointer data)
{
  BookmarkBox *box = data;
  const char *name = gtk_editable_get_text (GTK_EDITABLE (box->name));

  (void) button;
  if (!w42_view_go_to_bookmark (box->view, name))
    gtk_label_set_text (GTK_LABEL (box->status), "No bookmark of that name.");
}

static void
on_bookmark_delete (GtkButton *button, gpointer data)
{
  BookmarkBox *box = data;
  const char *name = gtk_editable_get_text (GTK_EDITABLE (box->name));

  (void) button;
  if (w42_view_go_to_bookmark (box->view, name))
    {
      w42_view_set_bookmark (box->view, NULL);
      bookmark_refresh (box);
    }
}

static void
on_bookmark_row (GtkListBox *list, GtkListBoxRow *row, gpointer data)
{
  BookmarkBox *box = data;
  GtkWidget *label;

  (void) list;
  if (row == NULL)
    return;
  label = gtk_list_box_row_get_child (row);
  if (GTK_IS_LABEL (label))
    gtk_editable_set_text (GTK_EDITABLE (box->name), gtk_label_get_text (GTK_LABEL (label)));
}

void
w42_bookmark_dialog_show (GtkWindow *parent, W42View *view)
{
  BookmarkBox *box;
  GtkWidget *content, *grid, *label, *scroller, *buttons, *add, *go, *del, *close;

  g_return_if_fail (W42_IS_VIEW (view));

  if (w42_view_get_document (view) == NULL)
    return;

  box = g_new0 (BookmarkBox, 1);
  box->view = view;
  box->window = dialog_shell (parent, "Bookmark", &content, view);
  gtk_window_set_modal (GTK_WINDOW (box->window), FALSE);
  g_object_weak_ref (G_OBJECT (box->window), bookmark_free, box);

  grid = group (content, "Bookmarks");

  label = gtk_label_new_with_mnemonic ("Bookmark _Name:");
  box->name = gtk_entry_new ();
  gtk_widget_set_size_request (box->name, 220, -1);
  gtk_entry_set_activates_default (GTK_ENTRY (box->name), TRUE);
  gtk_label_set_xalign (GTK_LABEL (label), 0.0);
  gtk_label_set_mnemonic_widget (GTK_LABEL (label), box->name);
  gtk_grid_attach (GTK_GRID (grid), label, 0, 0, 1, 1);
  gtk_grid_attach (GTK_GRID (grid), box->name, 1, 0, 1, 1);

  box->list = gtk_list_box_new ();
  gtk_list_box_set_selection_mode (GTK_LIST_BOX (box->list), GTK_SELECTION_SINGLE);
  g_signal_connect (box->list, "row-selected", G_CALLBACK (on_bookmark_row), box);
  scroller = gtk_scrolled_window_new ();
  gtk_scrolled_window_set_policy (GTK_SCROLLED_WINDOW (scroller),
                                  GTK_POLICY_NEVER, GTK_POLICY_AUTOMATIC);
  gtk_scrolled_window_set_has_frame (GTK_SCROLLED_WINDOW (scroller), TRUE);
  gtk_scrolled_window_set_child (GTK_SCROLLED_WINDOW (scroller), box->list);
  gtk_widget_set_size_request (scroller, -1, 120);
  gtk_grid_attach (GTK_GRID (grid), scroller, 0, 1, 2, 1);

  buttons = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 8);
  add = gtk_button_new_with_mnemonic ("_Add");
  go = gtk_button_new_with_mnemonic ("_Go To");
  del = gtk_button_new_with_mnemonic ("_Delete");
  g_signal_connect (add, "clicked", G_CALLBACK (on_bookmark_add), box);
  g_signal_connect (go, "clicked", G_CALLBACK (on_bookmark_go_to), box);
  g_signal_connect (del, "clicked", G_CALLBACK (on_bookmark_delete), box);
  gtk_box_append (GTK_BOX (buttons), add);
  gtk_box_append (GTK_BOX (buttons), go);
  gtk_box_append (GTK_BOX (buttons), del);
  gtk_grid_attach (GTK_GRID (grid), buttons, 0, 2, 2, 1);

  box->status = gtk_label_new ("");
  gtk_label_set_xalign (GTK_LABEL (box->status), 0.0);
  gtk_widget_add_css_class (box->status, "w42-dialog-status");
  gtk_grid_attach (GTK_GRID (grid), box->status, 0, 3, 2, 1);

  {
    GtkWidget *row = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 8);

    gtk_widget_set_halign (row, GTK_ALIGN_END);
    close = gtk_button_new_with_mnemonic ("Close");
    gtk_widget_set_size_request (close, 92, 26);
    g_signal_connect_swapped (close, "clicked", G_CALLBACK (gtk_window_destroy), box->window);
    gtk_box_append (GTK_BOX (row), close);
    gtk_box_append (GTK_BOX (content), row);
    gtk_window_set_default_widget (GTK_WINDOW (box->window), add);
  }

  bookmark_refresh (box);
  gtk_window_present (GTK_WINDOW (box->window));
  gtk_widget_grab_focus (box->name);
}

/* ---------------------------------------------------------------------- */
/* Font effects                                                            */
/* ---------------------------------------------------------------------- */

typedef struct {
  GtkWidget *window;
  W42View   *view;
  GtkWidget *strike, *super, *sub, *smallcaps, *allcaps, *overline;
  GtkWidget *underline;
  GtkWidget *highlight;
  GtkWidget *spacing;
  GtkWidget *colour;
} EffectsBox;

/* Word 6 offered the first four; the rest are what the file formats can
 * say, and what other programs write. */
static const char * const UNDERLINES[] = {
  "(none)", "Single", "Double", "Words Only", "Dotted", "Dashed", "Thick", "Wave", NULL
};

static const char * const HIGHLIGHTS[] = {
  "None", "Yellow", "Bright Green", "Turquoise", "Pink", "Blue", "Red",
  "Dark Blue", "Teal", "Green", "Violet", "Dark Red", "Dark Yellow", "Gray 50%",
  "Gray 25%", NULL
};
static const guint8 HIGHLIGHT_INDEX[] = { 0, 7, 4, 3, 5, 2, 6, 9, 10, 11, 12, 13, 14, 15, 16 };

/* Word 6's sixteen colours, as its Font box listed them; Auto is black. */
static const char *const TEXT_COLOURS[] = {
  "Auto", "Black", "Blue", "Cyan", "Green", "Magenta", "Red", "Yellow", "White",
  "Dark Blue", "Dark Cyan", "Dark Green", "Dark Magenta", "Dark Red", "Dark Yellow", "Dark Gray", "Light Gray", NULL
};
static const guint32 TEXT_COLOUR_VALUES[] = {
  0x000000, 0x000000, 0x0000FF, 0x00FFFF, 0x00FF00, 0xFF00FF, 0xFF0000, 0xFFFF00, 0xFFFFFF,
  0x000080, 0x008080, 0x008000, 0x800080, 0x800000, 0x808000, 0x808080, 0xC0C0C0
};

static void
on_effects_ok (GtkButton *button, gpointer data)
{
  EffectsBox *box = data;
  W42CharFmt want;
  guint h = gtk_drop_down_get_selected (GTK_DROP_DOWN (box->highlight));

  (void) button;

  memset (&want, 0, sizeof want);
  want.strikeout = gtk_check_button_get_active (GTK_CHECK_BUTTON (box->strike)) ? 1 : 0;
  want.overline  = gtk_check_button_get_active (GTK_CHECK_BUTTON (box->overline)) ? 1 : 0;
  want.script    = gtk_check_button_get_active (GTK_CHECK_BUTTON (box->super)) ? 1
                 : gtk_check_button_get_active (GTK_CHECK_BUTTON (box->sub)) ? -1 : 0;
  want.smallcaps = gtk_check_button_get_active (GTK_CHECK_BUTTON (box->smallcaps)) ? 1 : 0;
  want.allcaps   = gtk_check_button_get_active (GTK_CHECK_BUTTON (box->allcaps)) ? 1 : 0;
  want.highlight = h < G_N_ELEMENTS (HIGHLIGHT_INDEX) ? HIGHLIGHT_INDEX[h] : 0;
  want.underline = gtk_drop_down_get_selected (GTK_DROP_DOWN (box->underline));
  want.spacing   = (gint16) lround (gtk_spin_button_get_value (GTK_SPIN_BUTTON (box->spacing)) * 20.0);
  {
    guint c = gtk_drop_down_get_selected (GTK_DROP_DOWN (box->colour));

    want.color = c < G_N_ELEMENTS (TEXT_COLOUR_VALUES) ? TEXT_COLOUR_VALUES[c] : 0;
  }

  w42_view_apply_char_fmt (box->view,
                           W42_CHAR_STRIKEOUT | W42_CHAR_OVERLINE | W42_CHAR_SCRIPT | W42_CHAR_SMALLCAPS |
                           W42_CHAR_ALLCAPS | W42_CHAR_HIGHLIGHT | W42_CHAR_SPACING | W42_CHAR_COLOR |
                           W42_CHAR_UNDERLINE, &want);
  gtk_window_destroy (GTK_WINDOW (box->window));
}

static void
on_effects_exclusive (GtkCheckButton *button, gpointer data)
{
  /* Superscript and subscript cannot both be on. */
  GtkCheckButton *other = data;

  if (gtk_check_button_get_active (button))
    gtk_check_button_set_active (other, FALSE);
}

void
w42_effects_dialog_show (GtkWindow *parent, W42View *view)
{
  EffectsBox *box;
  GtkWidget *content, *grid;
  W42CharFmt now;
  guint h_index = 0;

  g_return_if_fail (W42_IS_VIEW (view));

  if (w42_view_get_document (view) == NULL)
    return;

  box = g_new0 (EffectsBox, 1);
  box->view = view;
  box->window = dialog_shell (parent, "Font Effects", &content, view);
  g_object_weak_ref (G_OBJECT (box->window), hf_free, box);
  w42_view_get_char_fmt (view, &now);

  grid = group (content, "Effects");
  box->strike    = gtk_check_button_new_with_mnemonic ("Stri_kethrough");
  box->super     = gtk_check_button_new_with_mnemonic ("Su_perscript");
  box->sub       = gtk_check_button_new_with_mnemonic ("Su_bscript");
  box->smallcaps = gtk_check_button_new_with_mnemonic ("S_mall Caps");
  box->allcaps   = gtk_check_button_new_with_mnemonic ("_All Caps");
  box->overline  = gtk_check_button_new_with_mnemonic ("O_verline");
  gtk_check_button_set_active (GTK_CHECK_BUTTON (box->overline), now.overline);
  gtk_check_button_set_active (GTK_CHECK_BUTTON (box->strike), now.strikeout);
  gtk_check_button_set_active (GTK_CHECK_BUTTON (box->super), now.script > 0);
  gtk_check_button_set_active (GTK_CHECK_BUTTON (box->sub), now.script < 0);
  gtk_check_button_set_active (GTK_CHECK_BUTTON (box->smallcaps), now.smallcaps);
  gtk_check_button_set_active (GTK_CHECK_BUTTON (box->allcaps), now.allcaps);
  g_signal_connect (box->super, "toggled", G_CALLBACK (on_effects_exclusive), box->sub);
  g_signal_connect (box->sub, "toggled", G_CALLBACK (on_effects_exclusive), box->super);
  gtk_grid_attach (GTK_GRID (grid), box->strike, 0, 0, 1, 1);
  gtk_grid_attach (GTK_GRID (grid), box->smallcaps, 1, 0, 1, 1);
  gtk_grid_attach (GTK_GRID (grid), box->super, 0, 1, 1, 1);
  gtk_grid_attach (GTK_GRID (grid), box->allcaps, 1, 1, 1, 1);
  gtk_grid_attach (GTK_GRID (grid), box->sub, 0, 2, 1, 1);
  gtk_grid_attach (GTK_GRID (grid), box->overline, 1, 2, 1, 1);

  grid = group (content, "Underline");
  box->underline = choice_row (grid, 0, 0, "_Underline:", UNDERLINES,
                               MIN (now.underline, G_N_ELEMENTS (UNDERLINES) - 2));

  grid = group (content, "Highlight");
  for (guint i = 0; i < G_N_ELEMENTS (HIGHLIGHT_INDEX); i++)
    if (HIGHLIGHT_INDEX[i] == now.highlight)
      h_index = i;
  box->highlight = choice_row (grid, 0, 0, "_Highlight:", HIGHLIGHTS, h_index);
  {
    guint ci = 0;

    for (guint i = 1; i < G_N_ELEMENTS (TEXT_COLOUR_VALUES); i++)
      if (TEXT_COLOUR_VALUES[i] == now.color && now.color != 0) ci = i;
    box->colour = choice_row (grid, 1, 0, "_Color:", TEXT_COLOURS, ci);
  }

  grid = group (content, "Character Spacing");
  {
    GtkWidget *label = gtk_label_new_with_mnemonic ("_Expanded by (pt):");

    box->spacing = gtk_spin_button_new_with_range (-10.0, 30.0, 0.25);
    gtk_spin_button_set_digits (GTK_SPIN_BUTTON (box->spacing), 2);
    gtk_spin_button_set_value (GTK_SPIN_BUTTON (box->spacing), now.spacing / 20.0);
    gtk_spin_button_set_activates_default (GTK_SPIN_BUTTON (box->spacing), TRUE);
    gtk_label_set_xalign (GTK_LABEL (label), 0.0);
    gtk_label_set_mnemonic_widget (GTK_LABEL (label), box->spacing);
    gtk_grid_attach (GTK_GRID (grid), label, 0, 0, 1, 1);
    gtk_grid_attach (GTK_GRID (grid), box->spacing, 1, 0, 1, 1);
  }

  button_row (content, box->window, G_CALLBACK (on_effects_ok), box);
  gtk_window_present (GTK_WINDOW (box->window));
  gtk_widget_grab_focus (box->strike);
}

/* ---------------------------------------------------------------------- */
/* Columns                                                                 */
/* ---------------------------------------------------------------------- */

typedef struct {
  GtkWidget *window;
  W42View   *view;
  GtkWidget *count;
  GtkWidget *gap;
  GtkWidget *scope;
} ColumnsBox;

static const char * const COLUMN_SCOPES[] = { "Whole document", "This section", "This point forward", NULL };

static const char * const COLUMN_PRESETS[] = { "One", "Two", "Three", NULL };

static void
on_columns_ok (GtkButton *button, gpointer data)
{
  ColumnsBox *box = data;
  W42Document *doc = w42_view_get_document (box->view);
  W42PageSetup page;

  (void) button;

  if (doc == NULL)
    return;

  page = *w42_document_page_setup (doc);
  page.columns = (int) gtk_drop_down_get_selected (GTK_DROP_DOWN (box->count)) + 1;
  page.column_gap = (int) lround (twips_from_measure (gtk_spin_button_get_value (GTK_SPIN_BUTTON (box->gap))));
  if (page.column_gap < 72)
    page.column_gap = 72;

  w42_view_set_columns (box->view, page.columns, page.column_gap,
                        (W42ColumnsScope) gtk_drop_down_get_selected (GTK_DROP_DOWN (box->scope)));
  gtk_window_destroy (GTK_WINDOW (box->window));
}

void
w42_columns_dialog_show (GtkWindow *parent, W42View *view)
{
  ColumnsBox *box;
  GtkWidget *content, *grid;
  const W42PageSetup *page;

  g_return_if_fail (W42_IS_VIEW (view));

  if (w42_view_get_document (view) == NULL)
    return;

  page = w42_document_page_setup (w42_view_get_document (view));
  box = g_new0 (ColumnsBox, 1);
  box->view = view;
  box->window = dialog_shell (parent, "Columns", &content, view);
  g_object_weak_ref (G_OBJECT (box->window), hf_free, box);

  grid = group (content, "Presets");
  {
    int columns = 1, gap = 720;

    (void) page;
    w42_view_get_columns (view, &columns, &gap);
    box->count = choice_row (grid, 0, 0, "_Number of Columns:", COLUMN_PRESETS,
                             CLAMP (columns - 1, 0, 2));
    box->gap = inches_row (grid, 1, 0, "_Spacing:", measure_from_twips (gap));
  }

  box->scope = choice_row (grid, 2, 0, "_Apply To:", COLUMN_SCOPES, 0);

  button_row (content, box->window, G_CALLBACK (on_columns_ok), box);
  gtk_window_present (GTK_WINDOW (box->window));
  gtk_widget_grab_focus (box->count);
}

/* ---------------------------------------------------------------------- */
/* Annotations                                                             */
/* ---------------------------------------------------------------------- */

typedef struct {
  GtkWidget *window;
  W42View   *view;
  GtkWidget *text;       /* GtkTextView for the annotation being written */
  GtkWidget *list;       /* GtkListBox of the document's annotations */
  GtkWidget *status;
  GArray    *items;      /* W42Annotation, as listed */
} AnnotationsBox;

static void
annotations_free (gpointer data, GObject *gone)
{
  AnnotationsBox *box = data;

  (void) gone;
  if (box->items != NULL)
    g_array_free (box->items, TRUE);
  g_free (box);
}

static char *
annotations_text (AnnotationsBox *box)
{
  GtkTextBuffer *buffer = gtk_text_view_get_buffer (GTK_TEXT_VIEW (box->text));
  GtkTextIter a, b;

  gtk_text_buffer_get_bounds (buffer, &a, &b);
  return gtk_text_buffer_get_text (buffer, &a, &b, FALSE);
}

static void
annotations_refresh (AnnotationsBox *box)
{
  GtkWidget *row;
  W42PieceTable *pt = w42_document_pt (w42_view_get_document (box->view));

  while ((row = gtk_widget_get_first_child (box->list)) != NULL)
    gtk_list_box_remove (GTK_LIST_BOX (box->list), row);

  if (box->items != NULL)
    g_array_free (box->items, TRUE);
  box->items = w42_pt_annotations (pt);

  for (guint i = 0; i < box->items->len; i++)
    {
      const W42Annotation *a = &g_array_index (box->items, W42Annotation, i);
      char *quoted = w42_pt_get_text (pt, a->start, MIN (a->end - a->start, 40));
      char *line;
      GtkWidget *label;

      g_strdelimit (quoted, "\n\t", ' ');
      line = g_strdup_printf ("\342\200\234%s%s\342\200\235 \342\200\224 %s", quoted,
                              a->end - a->start > 40 ? "\342\200\246" : "", a->text);
      label = gtk_label_new (line);
      gtk_label_set_xalign (GTK_LABEL (label), 0.0);
      gtk_label_set_ellipsize (GTK_LABEL (label), PANGO_ELLIPSIZE_END);
      gtk_label_set_max_width_chars (GTK_LABEL (label), 60);
      gtk_widget_set_margin_start (label, 4);
      gtk_list_box_append (GTK_LIST_BOX (box->list), label);
      g_free (line);
      g_free (quoted);
    }

  gtk_label_set_text (GTK_LABEL (box->status),
                      box->items->len == 0 ? "No annotations in the document." : "");
}

static void
on_annotation_add (GtkButton *button, gpointer data)
{
  AnnotationsBox *box = data;
  char *text = annotations_text (box);

  (void) button;

  if (!w42_view_has_selection (box->view))
    gtk_label_set_text (GTK_LABEL (box->status), "Select the text to annotate first.");
  else if (*g_strstrip (text) == '\0')
    gtk_label_set_text (GTK_LABEL (box->status), "Type the annotation first.");
  else
    {
      w42_view_set_comment (box->view, text);
      gtk_text_buffer_set_text (gtk_text_view_get_buffer (GTK_TEXT_VIEW (box->text)), "", -1);
      annotations_refresh (box);
    }
  g_free (text);
}

static void
on_annotation_row (GtkListBox *list, GtkListBoxRow *row, gpointer data)
{
  AnnotationsBox *box = data;
  int i;

  (void) list;
  if (row == NULL || box->items == NULL)
    return;
  i = gtk_list_box_row_get_index (row);
  if (i < 0 || (guint) i >= box->items->len)
    return;

  {
    const W42Annotation *a = &g_array_index (box->items, W42Annotation, i);

    w42_view_select_range (box->view, a->start, a->end);
    gtk_text_buffer_set_text (gtk_text_view_get_buffer (GTK_TEXT_VIEW (box->text)), a->text, -1);
  }
}

static void
on_annotation_delete (GtkButton *button, gpointer data)
{
  AnnotationsBox *box = data;
  GtkListBoxRow *row = gtk_list_box_get_selected_row (GTK_LIST_BOX (box->list));
  int i;

  (void) button;
  if (row == NULL || box->items == NULL)
    return;
  i = gtk_list_box_row_get_index (row);
  if (i < 0 || (guint) i >= box->items->len)
    return;

  {
    const W42Annotation *a = &g_array_index (box->items, W42Annotation, i);

    w42_view_select_range (box->view, a->start, a->end);
    w42_view_set_comment (box->view, NULL);
    annotations_refresh (box);
  }
}

void
w42_annotations_dialog_show (GtkWindow *parent, W42View *view)
{
  AnnotationsBox *box;
  GtkWidget *content, *grid, *label, *scroller, *buttons, *add, *del, *close;
  const char *current;

  g_return_if_fail (W42_IS_VIEW (view));

  if (w42_view_get_document (view) == NULL)
    return;

  box = g_new0 (AnnotationsBox, 1);
  box->view = view;
  box->window = dialog_shell (parent, "Annotations", &content, view);
  gtk_window_set_modal (GTK_WINDOW (box->window), FALSE);
  g_object_weak_ref (G_OBJECT (box->window), annotations_free, box);

  grid = group (content, "Annotation");

  label = gtk_label_new_with_mnemonic ("_Text:");
  gtk_label_set_xalign (GTK_LABEL (label), 0.0);
  gtk_grid_attach (GTK_GRID (grid), label, 0, 0, 2, 1);

  box->text = gtk_text_view_new ();
  gtk_text_view_set_wrap_mode (GTK_TEXT_VIEW (box->text), GTK_WRAP_WORD);
  gtk_label_set_mnemonic_widget (GTK_LABEL (label), box->text);
  scroller = gtk_scrolled_window_new ();
  gtk_scrolled_window_set_has_frame (GTK_SCROLLED_WINDOW (scroller), TRUE);
  gtk_scrolled_window_set_child (GTK_SCROLLED_WINDOW (scroller), box->text);
  gtk_widget_set_size_request (scroller, 360, 70);
  gtk_grid_attach (GTK_GRID (grid), scroller, 0, 1, 2, 1);

  current = w42_view_get_comment (view);
  if (current != NULL)
    gtk_text_buffer_set_text (gtk_text_view_get_buffer (GTK_TEXT_VIEW (box->text)), current, -1);

  grid = group (content, "In the Document");
  box->list = gtk_list_box_new ();
  gtk_list_box_set_selection_mode (GTK_LIST_BOX (box->list), GTK_SELECTION_SINGLE);
  g_signal_connect (box->list, "row-selected", G_CALLBACK (on_annotation_row), box);
  scroller = gtk_scrolled_window_new ();
  gtk_scrolled_window_set_policy (GTK_SCROLLED_WINDOW (scroller),
                                  GTK_POLICY_NEVER, GTK_POLICY_AUTOMATIC);
  gtk_scrolled_window_set_has_frame (GTK_SCROLLED_WINDOW (scroller), TRUE);
  gtk_scrolled_window_set_child (GTK_SCROLLED_WINDOW (scroller), box->list);
  gtk_widget_set_size_request (scroller, 360, 120);
  gtk_grid_attach (GTK_GRID (grid), scroller, 0, 0, 2, 1);

  box->status = gtk_label_new ("");
  gtk_label_set_xalign (GTK_LABEL (box->status), 0.0);
  gtk_widget_add_css_class (box->status, "w42-dialog-status");
  gtk_grid_attach (GTK_GRID (grid), box->status, 0, 1, 2, 1);

  buttons = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 8);
  gtk_widget_set_halign (buttons, GTK_ALIGN_END);
  add = gtk_button_new_with_mnemonic ("_Add");
  del = gtk_button_new_with_mnemonic ("_Delete");
  close = gtk_button_new_with_mnemonic ("Close");
  gtk_widget_set_size_request (add, 92, 26);
  gtk_widget_set_size_request (del, 92, 26);
  gtk_widget_set_size_request (close, 92, 26);
  g_signal_connect (add, "clicked", G_CALLBACK (on_annotation_add), box);
  g_signal_connect (del, "clicked", G_CALLBACK (on_annotation_delete), box);
  g_signal_connect_swapped (close, "clicked", G_CALLBACK (gtk_window_destroy), box->window);
  gtk_box_append (GTK_BOX (buttons), add);
  gtk_box_append (GTK_BOX (buttons), del);
  gtk_box_append (GTK_BOX (buttons), close);
  gtk_box_append (GTK_BOX (content), buttons);

  annotations_refresh (box);
  gtk_window_present (GTK_WINDOW (box->window));
  gtk_widget_grab_focus (box->text);
}

/* ---------------------------------------------------------------------- */
/* Tools > Mail Merge                                                      */
/* ---------------------------------------------------------------------- */

typedef struct {
  W42View        *view;
  GtkWidget      *window;
  GtkWidget      *path;
  GtkWidget      *list;
  GtkWidget      *status;
  W42MergeSource *source;
} MergeBox;

static void
merge_free (gpointer data, GObject *where)
{
  MergeBox *box = data;

  (void) where;
  w42_merge_source_free (box->source);
  g_free (box);
}

static const char *
merge_selected_field (MergeBox *box)
{
  GtkListBoxRow *row = gtk_list_box_get_selected_row (GTK_LIST_BOX (box->list));
  GtkWidget *label;

  if (row == NULL)
    return NULL;
  label = gtk_list_box_row_get_child (row);
  return GTK_IS_LABEL (label) ? gtk_label_get_text (GTK_LABEL (label)) : NULL;
}

static void
merge_show_source (MergeBox *box)
{
  GtkWidget *child;

  while ((child = gtk_widget_get_first_child (box->list)) != NULL)
    gtk_list_box_remove (GTK_LIST_BOX (box->list), child);

  if (box->source == NULL)
    return;

  for (guint i = 0; box->source->fields[i] != NULL; i++)
    {
      GtkWidget *label = gtk_label_new (box->source->fields[i]);

      gtk_label_set_xalign (GTK_LABEL (label), 0.0);
      gtk_list_box_append (GTK_LIST_BOX (box->list), label);
    }
  gtk_list_box_select_row (GTK_LIST_BOX (box->list),
                           gtk_list_box_get_row_at_index (GTK_LIST_BOX (box->list), 0));

  {
    char *text = g_strdup_printf ("%u record%s, %u field%s.",
                                  box->source->rows->len,
                                  box->source->rows->len == 1 ? "" : "s",
                                  g_strv_length (box->source->fields),
                                  g_strv_length (box->source->fields) == 1 ? "" : "s");
    gtk_label_set_text (GTK_LABEL (box->status), text);
    g_free (text);
  }
}

static void
on_merge_source_chosen (GObject *object, GAsyncResult *result, gpointer data)
{
  MergeBox *box = data;
  GError *error = NULL;
  GFile *file = gtk_file_dialog_open_finish (GTK_FILE_DIALOG (object), result, &error);
  W42MergeSource *source;

  if (file == NULL)
    {
      g_clear_error (&error);
      return;
    }

  source = w42_merge_source_load (file, &error);
  if (source == NULL)
    {
      gtk_label_set_text (GTK_LABEL (box->status), error->message);
      g_error_free (error);
    }
  else
    {
      char *name = g_file_get_basename (file);

      w42_merge_source_free (box->source);
      box->source = source;
      gtk_label_set_text (GTK_LABEL (box->path), name);
      g_free (name);
      merge_show_source (box);
    }
  g_object_unref (file);
}

static void
on_merge_open_source (GtkButton *button, gpointer data)
{
  MergeBox *box = data;
  GtkFileDialog *dialog = gtk_file_dialog_new ();
  GListStore *filters = g_list_store_new (GTK_TYPE_FILE_FILTER);
  GtkFileFilter *csv = gtk_file_filter_new ();

  (void) button;
  gtk_file_filter_set_name (csv, "Comma Separated Values (*.csv)");
  gtk_file_filter_add_pattern (csv, "*.csv");
  gtk_file_filter_add_pattern (csv, "*.txt");
  g_list_store_append (filters, csv);
  gtk_file_dialog_set_title (dialog, "Open Data Source");
  gtk_file_dialog_set_filters (dialog, G_LIST_MODEL (filters));
  gtk_file_dialog_open (dialog, GTK_WINDOW (box->window), NULL,
                        on_merge_source_chosen, box);
  g_object_unref (csv);
  g_object_unref (filters);
  g_object_unref (dialog);
}

static void
on_merge_insert_field (GtkButton *button, gpointer data)
{
  MergeBox *box = data;
  const char *name = merge_selected_field (box);
  char *text;

  (void) button;
  if (name == NULL)
    {
      gtk_label_set_text (GTK_LABEL (box->status), "Open a data source first.");
      return;
    }
  text = w42_merge_field_text (name);
  w42_view_insert_text (box->view, text);
  g_free (text);
}

static void
on_merge_output_chosen (GObject *object, GAsyncResult *result, gpointer data)
{
  MergeBox *box = data;
  GError *error = NULL;
  GFile *file = gtk_file_dialog_save_finish (GTK_FILE_DIALOG (object), result, &error);
  W42Document *doc = w42_view_get_document (box->view);
  GtkWindow *parent;

  if (file == NULL)
    {
      g_clear_error (&error);
      return;
    }

  if (!w42_merge_to_file (w42_document_pt (doc), w42_document_page_setup (doc),
                          box->source, file, &error))
    {
      gtk_label_set_text (GTK_LABEL (box->status), error->message);
      g_error_free (error);
      g_object_unref (file);
      return;
    }

  /* The result opens in a window of its own, as Word's Merge to New
   * Document did. */
  parent = gtk_window_get_transient_for (GTK_WINDOW (box->window));
  if (parent != NULL)
    {
      GtkApplication *app = gtk_window_get_application (parent);
      GtkWidget *window = w42_window_new (app);

      w42_window_open (W42_WINDOW (window), file);
      gtk_window_present (GTK_WINDOW (window));
    }
  gtk_window_destroy (GTK_WINDOW (box->window));
  g_object_unref (file);
}

static void
on_merge_run (GtkButton *button, gpointer data)
{
  MergeBox *box = data;
  GtkFileDialog *dialog;
  GListStore *filters;
  GtkFileFilter *rtf;

  (void) button;
  if (box->source == NULL)
    {
      gtk_label_set_text (GTK_LABEL (box->status), "Open a data source first.");
      return;
    }
  if (box->source->rows->len == 0)
    {
      gtk_label_set_text (GTK_LABEL (box->status), "The data source has no records.");
      return;
    }

  dialog = gtk_file_dialog_new ();
  filters = g_list_store_new (GTK_TYPE_FILE_FILTER);
  rtf = gtk_file_filter_new ();
  gtk_file_filter_set_name (rtf, "Rich Text Format (*.rtf)");
  gtk_file_filter_add_pattern (rtf, "*.rtf");
  g_list_store_append (filters, rtf);
  gtk_file_dialog_set_title (dialog, "Merge to New Document");
  gtk_file_dialog_set_filters (dialog, G_LIST_MODEL (filters));
  gtk_file_dialog_set_initial_name (dialog, "Merged.rtf");
  gtk_file_dialog_save (dialog, GTK_WINDOW (box->window), NULL,
                        on_merge_output_chosen, box);
  g_object_unref (rtf);
  g_object_unref (filters);
  g_object_unref (dialog);
}

void
w42_mail_merge_dialog_show (GtkWindow *parent, W42View *view)
{
  MergeBox *box;
  GtkWidget *content, *grid, *label, *scroller, *buttons, *open, *insert, *merge, *close;

  g_return_if_fail (W42_IS_VIEW (view));

  if (w42_view_get_document (view) == NULL)
    return;

  box = g_new0 (MergeBox, 1);
  box->view = view;
  box->window = dialog_shell (parent, "Mail Merge", &content, view);
  gtk_window_set_modal (GTK_WINDOW (box->window), FALSE);
  g_object_weak_ref (G_OBJECT (box->window), merge_free, box);

  grid = group (content, "Data Source");

  label = gtk_label_new ("A CSV file whose first row names the fields.");
  gtk_label_set_xalign (GTK_LABEL (label), 0.0);
  gtk_grid_attach (GTK_GRID (grid), label, 0, 0, 2, 1);

  open = gtk_button_new_with_mnemonic ("_Get Data...");
  g_signal_connect (open, "clicked", G_CALLBACK (on_merge_open_source), box);
  box->path = gtk_label_new ("(none)");
  gtk_label_set_xalign (GTK_LABEL (box->path), 0.0);
  gtk_label_set_ellipsize (GTK_LABEL (box->path), PANGO_ELLIPSIZE_MIDDLE);
  gtk_widget_set_hexpand (box->path, TRUE);
  gtk_grid_attach (GTK_GRID (grid), open, 0, 1, 1, 1);
  gtk_grid_attach (GTK_GRID (grid), box->path, 1, 1, 1, 1);

  grid = group (content, "Merge Fields");

  box->list = gtk_list_box_new ();
  gtk_list_box_set_selection_mode (GTK_LIST_BOX (box->list), GTK_SELECTION_SINGLE);
  scroller = gtk_scrolled_window_new ();
  gtk_scrolled_window_set_policy (GTK_SCROLLED_WINDOW (scroller),
                                  GTK_POLICY_NEVER, GTK_POLICY_AUTOMATIC);
  gtk_scrolled_window_set_has_frame (GTK_SCROLLED_WINDOW (scroller), TRUE);
  gtk_scrolled_window_set_child (GTK_SCROLLED_WINDOW (scroller), box->list);
  gtk_widget_set_size_request (scroller, 260, 120);
  gtk_grid_attach (GTK_GRID (grid), scroller, 0, 0, 2, 1);

  buttons = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 8);
  insert = gtk_button_new_with_mnemonic ("_Insert Merge Field");
  merge = gtk_button_new_with_mnemonic ("_Merge to New Document...");
  g_signal_connect (insert, "clicked", G_CALLBACK (on_merge_insert_field), box);
  g_signal_connect (merge, "clicked", G_CALLBACK (on_merge_run), box);
  gtk_box_append (GTK_BOX (buttons), insert);
  gtk_box_append (GTK_BOX (buttons), merge);
  gtk_grid_attach (GTK_GRID (grid), buttons, 0, 1, 2, 1);

  box->status = gtk_label_new ("Fields go into the text as \302\253Name\302\273.");
  gtk_label_set_xalign (GTK_LABEL (box->status), 0.0);
  gtk_widget_add_css_class (box->status, "w42-dialog-status");
  gtk_grid_attach (GTK_GRID (grid), box->status, 0, 2, 2, 1);

  {
    GtkWidget *row = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 8);

    gtk_widget_set_halign (row, GTK_ALIGN_END);
    close = gtk_button_new_with_mnemonic ("Close");
    gtk_widget_set_size_request (close, 92, 26);
    g_signal_connect_swapped (close, "clicked", G_CALLBACK (gtk_window_destroy), box->window);
    gtk_box_append (GTK_BOX (row), close);
    gtk_box_append (GTK_BOX (content), row);
  }

  gtk_window_present (GTK_WINDOW (box->window));
}

/* ---------------------------------------------------------------------- */
/* Insert > Cross-reference                                                */
/* ---------------------------------------------------------------------- */

typedef struct {
  W42View   *view;
  GtkWidget *window;
  GtkWidget *which;
  GtkWidget *kind;
  char     **names;
} XrefBox;

static void
xref_free (gpointer data, GObject *where)
{
  XrefBox *box = data;

  (void) where;
  g_strfreev (box->names);
  g_free (box);
}

static void
on_xref_ok (GtkButton *button, gpointer data)
{
  XrefBox *box = data;
  guint which = gtk_drop_down_get_selected (GTK_DROP_DOWN (box->which));
  guint kind = gtk_drop_down_get_selected (GTK_DROP_DOWN (box->kind));

  (void) button;
  if (which < g_strv_length (box->names))
    w42_view_insert_cross_reference (box->view, box->names[which], kind == 0);
  gtk_window_destroy (GTK_WINDOW (box->window));
}

void
w42_cross_reference_dialog_show (GtkWindow *parent, W42View *view)
{
  XrefBox *box;
  GtkWidget *content, *grid, *label;
  W42Document *doc;
  static const char * const kinds[] = { "Page number", "Bookmark text", NULL };

  g_return_if_fail (W42_IS_VIEW (view));

  doc = w42_view_get_document (view);
  if (doc == NULL)
    return;

  box = g_new0 (XrefBox, 1);
  box->view = view;
  box->names = w42_pt_bookmark_names (w42_document_pt (doc));
  box->window = dialog_shell (parent, "Cross-reference", &content, view);
  g_object_weak_ref (G_OBJECT (box->window), xref_free, box);

  grid = group (content, "Reference");

  if (box->names == NULL || box->names[0] == NULL)
    {
      label = gtk_label_new ("There are no bookmarks to refer to.\n"
                             "Insert > Bookmark marks a place first.");
      gtk_label_set_xalign (GTK_LABEL (label), 0.0);
      gtk_grid_attach (GTK_GRID (grid), label, 0, 0, 2, 1);
      box->which = choice_row (grid, 1, 0, "For which _bookmark:", NULL, 0);
      gtk_widget_set_sensitive (box->which, FALSE);
    }
  else
    box->which = choice_row (grid, 0, 0, "For which _bookmark:",
                             (const char * const *) box->names, 0);
  box->kind = choice_row (grid, 2, 0, "_Insert reference to:", kinds, 0);

  label = gtk_label_new ("The reference is text; insert it again after the "
                         "pages change.");
  gtk_label_set_xalign (GTK_LABEL (label), 0.0);
  gtk_widget_add_css_class (label, "w42-dialog-status");
  gtk_grid_attach (GTK_GRID (grid), label, 0, 3, 2, 1);

  button_row (content, box->window, G_CALLBACK (on_xref_ok), box);
  gtk_window_present (GTK_WINDOW (box->window));
}

/* ---------------------------------------------------------------------- */
/* Insert > Drawing                                                        */
/* ---------------------------------------------------------------------- */

typedef struct {
  GtkWidget *window;
  W42View   *view;
  GtkWidget *kind, *width, *height, *line, *line_colour, *filled, *fill_colour, *text;
  gboolean   editing;     /* Format > Drawing on the selected shape */
} DrawingBox;

static const char * const SHAPE_NAMES[] = {
  "Line", "Arrow", "Rectangle", "Rounded Rectangle", "Ellipse", NULL
};
static const char * const SHAPE_COLOURS[] = {
  "Black", "White", "Gray", "Red", "Green", "Blue", "Yellow", NULL
};
static const guint32 SHAPE_RGB[] = {
  0x000000, 0xffffff, 0x808080, 0xc00000, 0x008000, 0x0000c0, 0xffff00
};

/* Which entry of the shape colours a colour is, or the nearest. */
static guint
shape_colour_index (guint32 rgb)
{
  guint best = 0;
  long best_d = G_MAXLONG;

  for (guint i = 0; i < G_N_ELEMENTS (SHAPE_RGB); i++)
    {
      long dr = (long) ((rgb >> 16) & 0xFF) - (long) ((SHAPE_RGB[i] >> 16) & 0xFF);
      long dg = (long) ((rgb >> 8) & 0xFF) - (long) ((SHAPE_RGB[i] >> 8) & 0xFF);
      long db = (long) (rgb & 0xFF) - (long) (SHAPE_RGB[i] & 0xFF);
      long d = dr * dr + dg * dg + db * db;

      if (d < best_d)
        {
          best_d = d;
          best = i;
        }
    }
  return best;
}

static void
on_drawing_ok (GtkButton *button, gpointer data)
{
  DrawingBox *box = data;
  W42ShapeKind kind = (W42ShapeKind) (gtk_drop_down_get_selected (GTK_DROP_DOWN (box->kind)) + 1);
  int w = twips_from_measure (gtk_spin_button_get_value (GTK_SPIN_BUTTON (box->width)));
  int h = twips_from_measure (gtk_spin_button_get_value (GTK_SPIN_BUTTON (box->height)));
  double line_pt = gtk_spin_button_get_value (GTK_SPIN_BUTTON (box->line));
  guint lc = gtk_drop_down_get_selected (GTK_DROP_DOWN (box->line_colour));
  guint fc = gtk_drop_down_get_selected (GTK_DROP_DOWN (box->fill_colour));
  gboolean filled = gtk_check_button_get_active (GTK_CHECK_BUTTON (box->filled));
  const char *text = gtk_editable_get_text (GTK_EDITABLE (box->text));

  (void) button;

  if (kind >= W42_SHAPE_KINDS)
    kind = W42_SHAPE_RECTANGLE;
  if (box->editing)
    {
      w42_view_set_shape (box->view, kind, line_pt,
                          SHAPE_RGB[MIN (lc, G_N_ELEMENTS (SHAPE_RGB) - 1)],
                          filled, SHAPE_RGB[MIN (fc, G_N_ELEMENTS (SHAPE_RGB) - 1)], text);
      {
        int ow, oh;
        W42Wrap wrap;

        if (w42_view_get_picture (box->view, &ow, &oh, &wrap) && (ow != w || oh != h))
          w42_view_set_picture (box->view, MAX (w, 15), MAX (h, 15), wrap);
      }
    }
  else
    w42_view_insert_shape (box->view, kind, MAX (w, 72), MAX (h, 30), line_pt,
                           SHAPE_RGB[MIN (lc, G_N_ELEMENTS (SHAPE_RGB) - 1)],
                           filled, SHAPE_RGB[MIN (fc, G_N_ELEMENTS (SHAPE_RGB) - 1)], text);
  gtk_window_destroy (GTK_WINDOW (box->window));
}

void
w42_drawing_dialog_show (GtkWindow *parent, W42View *view)
{
  DrawingBox *box;
  GtkWidget *content, *grid;
  W42Object shown;

  memset (&shown, 0, sizeof shown);

  g_return_if_fail (W42_IS_VIEW (view));

  if (w42_view_get_document (view) == NULL)
    return;

  box = g_new0 (DrawingBox, 1);
  box->view = view;
  /* On a selected shape the dialog shows it as it is, and changes it. */
  {
    const W42Object *object = w42_view_get_object (view);

    if (object != NULL && object->shape != W42_SHAPE_PICTURE)
      {
        box->editing = TRUE;
        shown = *object;
      }
  }
  box->window = dialog_shell (parent, box->editing ? "Format Drawing" : "Drawing", &content, view);
  g_object_weak_ref (G_OBJECT (box->window), hf_free, box);

  grid = group (content, "Shape");
  box->kind   = choice_row (grid, 0, 0, "_Shape:", SHAPE_NAMES,
                            box->editing ? (guint) MAX ((int) shown.shape - 1, 0) : 2);
  box->width  = inches_row (grid, 1, 0, "_Width:", measure_from_twips (box->editing ? shown.width : 2 * 1440));
  box->height = inches_row (grid, 2, 0, "_Height:", measure_from_twips (box->editing ? shown.height : 1440));
  {
    GtkWidget *label = gtk_label_new_with_mnemonic ("_Text:");

    box->text = gtk_entry_new ();
    gtk_entry_set_placeholder_text (GTK_ENTRY (box->text), "set in the middle of the shape");
    if (box->editing && shown.text != NULL)
      gtk_editable_set_text (GTK_EDITABLE (box->text), shown.text);
    gtk_label_set_xalign (GTK_LABEL (label), 0.0);
    gtk_label_set_mnemonic_widget (GTK_LABEL (label), box->text);
    gtk_grid_attach (GTK_GRID (grid), label, 0, 3, 1, 1);
    gtk_grid_attach (GTK_GRID (grid), box->text, 1, 3, 1, 1);
  }

  grid = group (content, "Line and Fill");
  {
    GtkWidget *label = gtk_label_new_with_mnemonic ("_Line Width (pt):");

    box->line = gtk_spin_button_new_with_range (0.0, 12.0, 0.25);
    gtk_spin_button_set_value (GTK_SPIN_BUTTON (box->line), box->editing ? shown.line_pt : 1.0);
    gtk_label_set_xalign (GTK_LABEL (label), 0.0);
    gtk_label_set_mnemonic_widget (GTK_LABEL (label), box->line);
    gtk_grid_attach (GTK_GRID (grid), label, 0, 0, 1, 1);
    gtk_grid_attach (GTK_GRID (grid), box->line, 1, 0, 1, 1);
  }
  box->line_colour = choice_row (grid, 1, 0, "Line _Color:", SHAPE_COLOURS,
                                 box->editing ? shape_colour_index (shown.line_rgb) : 0);
  box->filled = gtk_check_button_new_with_mnemonic ("_Filled");
  gtk_check_button_set_active (GTK_CHECK_BUTTON (box->filled), box->editing && shown.filled);
  gtk_grid_attach (GTK_GRID (grid), box->filled, 0, 2, 2, 1);
  box->fill_colour = choice_row (grid, 3, 0, "Fill Colo_r:", SHAPE_COLOURS,
                                 box->editing ? shape_colour_index (shown.fill_rgb) : 6);

  button_row (content, box->window, G_CALLBACK (on_drawing_ok), box);
  gtk_window_present (GTK_WINDOW (box->window));
}

/* ---------------------------------------------------------------------- */
/* Format > Bullets and Numbering                                          */
/* ---------------------------------------------------------------------- */

typedef struct {
  GtkWidget *window;
  W42View   *view;
  GtkWidget *kind;
  GtkWidget *restart;
  GtkWidget *start;
  GtkWidget *level;
} ListBox;

/* In the order of W42ListKind. */
static const char * const LIST_KIND_NAMES[] = {
  "None", "\342\200\242 Bullet", "1. 2. 3.", "a. b. c.", "A. B. C.",
  "i. ii. iii.", "I. II. III.", "\342\227\246 Circle", "\342\226\252 Square",
  "\342\200\223 Dash", NULL
};

static void
on_list_kind_changed (GObject *drop, GParamSpec *pspec, gpointer data)
{
  ListBox *box = data;
  gboolean numbered = w42_list_is_numbered ((W42ListKind) gtk_drop_down_get_selected (GTK_DROP_DOWN (drop)));

  (void) pspec;
  gtk_widget_set_sensitive (box->restart, numbered);
  gtk_widget_set_sensitive (box->start, numbered);
}

static void
on_list_ok (GtkButton *button, gpointer data)
{
  ListBox *box = data;
  W42ListKind kind = (W42ListKind) gtk_drop_down_get_selected (GTK_DROP_DOWN (box->kind));
  gboolean restart = gtk_check_button_get_active (GTK_CHECK_BUTTON (box->restart));
  int start = (int) gtk_spin_button_get_value (GTK_SPIN_BUTTON (box->start));

  (void) button;
  w42_view_set_list (box->view, kind);
  if (kind != W42_LIST_NONE)
    {
      int level = (int) gtk_spin_button_get_value (GTK_SPIN_BUTTON (box->level)) - 1;
      W42ParaFmt now;

      w42_view_get_para_fmt (box->view, &now);
      if (level != (int) now.list_level)
        w42_view_list_level_by (box->view, level - (int) now.list_level);
    }
  if (w42_list_is_numbered (kind))
    w42_view_set_list_start (box->view, restart ? MAX (start, 1) : 0);
  gtk_window_destroy (GTK_WINDOW (box->window));
}

void
w42_list_dialog_show (GtkWindow *parent, W42View *view)
{
  ListBox *box;
  GtkWidget *content, *grid, *label;
  W42ParaFmt now;

  g_return_if_fail (W42_IS_VIEW (view));

  if (w42_view_get_document (view) == NULL)
    return;

  box = g_new0 (ListBox, 1);
  box->view = view;
  box->window = dialog_shell (parent, "Bullets and Numbering", &content, view);
  g_object_weak_ref (G_OBJECT (box->window), hf_free, box);
  w42_view_get_para_fmt (view, &now);

  grid = group (content, "List");
  box->kind = choice_row (grid, 0, 0, "_Kind:", LIST_KIND_NAMES,
                          MIN (now.list, W42_LIST_KINDS - 1));

  {
    GtkWidget *lvl_label = gtk_label_new_with_mnemonic ("_Level:");

    box->level = gtk_spin_button_new_with_range (1, 9, 1);
    gtk_spin_button_set_value (GTK_SPIN_BUTTON (box->level), now.list_level + 1);
    gtk_label_set_xalign (GTK_LABEL (lvl_label), 0.0);
    gtk_label_set_mnemonic_widget (GTK_LABEL (lvl_label), box->level);
    gtk_grid_attach (GTK_GRID (grid), lvl_label, 0, 3, 1, 1);
    gtk_grid_attach (GTK_GRID (grid), box->level, 1, 3, 1, 1);
  }
  box->restart = gtk_check_button_new_with_mnemonic ("_Restart numbering at:");
  gtk_check_button_set_active (GTK_CHECK_BUTTON (box->restart), now.list_start > 0);
  box->start = gtk_spin_button_new_with_range (1, 255, 1);
  gtk_spin_button_set_value (GTK_SPIN_BUTTON (box->start), now.list_start > 0 ? now.list_start : 1);
  gtk_grid_attach (GTK_GRID (grid), box->restart, 0, 1, 1, 1);
  gtk_grid_attach (GTK_GRID (grid), box->start, 1, 1, 1, 1);

  label = gtk_label_new ("Numbering continues from the item before unless restarted.\n"
                         "Tab and Shift+Tab at the start of an item change its level.");
  gtk_label_set_xalign (GTK_LABEL (label), 0.0);
  gtk_widget_add_css_class (label, "w42-dialog-status");
  gtk_grid_attach (GTK_GRID (grid), label, 0, 4, 2, 1);

  g_signal_connect (box->kind, "notify::selected", G_CALLBACK (on_list_kind_changed), box);
  on_list_kind_changed (G_OBJECT (box->kind), NULL, box);

  button_row (content, box->window, G_CALLBACK (on_list_ok), box);
  gtk_window_present (GTK_WINDOW (box->window));
}

/* ---------------------------------------------------------------------- */
/* Table > Table Properties                                                */
/* ---------------------------------------------------------------------- */

typedef struct {
  GtkWidget *window;
  W42View   *view;
  GtkWidget *borders;
  GtkWidget *header;
  GtkWidget *row_height;
  GtkWidget *shading;
  GtkWidget *fill;
  GtkWidget *side[4];      /* top, bottom, left, right */
  int        sides_before;
  GtkWidget *style, *width, *colour;   /* the cell's sides' line */
  GtkWidget *valign;
  W42ParaFmt cell_before;
  gboolean   have_cell;
} TablePropsBox;

static const char * const VALIGN_NAMES[] = { "Top", "Center", "Bottom", NULL };

/* An inches (or cm) spinner's value in twips. */
static int
spin_twips (GtkWidget *spin)
{
  double v = gtk_spin_button_get_value (GTK_SPIN_BUTTON (spin));

  if (w42_settings_get_units () == W42_UNITS_CM)
    v /= 2.54;
  return (int) lround (v * 1440.0);
}

static void
on_table_props_ok (GtkButton *button, gpointer data)
{
  TablePropsBox *box = data;
  guint sh = gtk_drop_down_get_selected (GTK_DROP_DOWN (box->shading));
  W42ParaFmt now;

  (void) button;
  w42_view_table_set_borders (box->view, gtk_check_button_get_active (GTK_CHECK_BUTTON (box->borders)));
  w42_view_table_set_header_rows (box->view, gtk_check_button_get_active (GTK_CHECK_BUTTON (box->header)) ? 1 : 0);
  w42_view_table_set_row_height (box->view, spin_twips (box->row_height));
  w42_view_get_para_fmt (box->view, &now);
  if ((int) SHADING_VALUES[MIN (sh, 7)] != (int) now.shading)
    w42_view_cell_set_shading (box->view, SHADING_VALUES[MIN (sh, 7)]);
  {
    guint bg = gtk_drop_down_get_selected (GTK_DROP_DOWN (box->fill));
    guint32 was = 0;
    gboolean had = w42_view_cell_get_fill (box->view, &was);

    if (bg > 0)
      {
        guint32 want_rgb = PALETTE_VALUES[MIN (bg - 1, G_N_ELEMENTS (PALETTE_VALUES) - 1)];

        if (!had || was != want_rgb)
          w42_view_cell_set_fill (box->view, TRUE, want_rgb);
      }
    else if (had)
      w42_view_cell_set_fill (box->view, FALSE, 0);
  }
  {
    static const int bits[4] = { W42_BORDER_TOP, W42_BORDER_BOTTOM, W42_BORDER_LEFT, W42_BORDER_RIGHT };
    int sides = 0;
    guint st = gtk_drop_down_get_selected (GTK_DROP_DOWN (box->style));
    guint w = gtk_drop_down_get_selected (GTK_DROP_DOWN (box->width));
    guint lc = gtk_drop_down_get_selected (GTK_DROP_DOWN (box->colour));
    guint va = gtk_drop_down_get_selected (GTK_DROP_DOWN (box->valign));
    W42ParaFmt cell;

    for (int i = 0; i < 4; i++)
      if (gtk_check_button_get_active (GTK_CHECK_BUTTON (box->side[i])))
        sides |= bits[i];
    if (box->have_cell && w42_view_cell_get_fmt (box->view, &cell))
      {
        W42ParaFmt want = cell;

        want.border = (guint8) (W42_BORDER_CELL_SET | sides);
        w42_para_fmt_set_edges (&want, BORDER_WIDTH_TWIPS[MIN (w, G_N_ELEMENTS (BORDER_WIDTH_TWIPS) - 1)],
                                PALETTE_VALUES[MIN (lc, G_N_ELEMENTS (PALETTE_VALUES) - 1)],
                                (W42BorderStyle) MIN (st, W42_BORDER_DOTTED));
        want.cell_valign = (guint8) MIN (va, W42_CELL_VALIGN_BOTTOM);
        if (memcmp (&want, &cell, sizeof want) != 0)
          w42_view_cell_set_fmt (box->view, &want);
      }
    else if (sides != box->sides_before)
      w42_view_cell_set_borders (box->view, sides);
  }
  gtk_window_destroy (GTK_WINDOW (box->window));
}

void
w42_table_properties_dialog_show (GtkWindow *parent, W42View *view)
{
  TablePropsBox *box;
  GtkWidget *content, *grid;
  W42ParaFmt now;
  guint shading_index = 0;

  g_return_if_fail (W42_IS_VIEW (view));

  if (w42_view_get_document (view) == NULL)
    return;

  box = g_new0 (TablePropsBox, 1);
  box->view = view;
  box->window = dialog_shell (parent, "Table Properties", &content, view);
  g_object_weak_ref (G_OBJECT (box->window), hf_free, box);
  w42_view_get_para_fmt (view, &now);

  grid = group (content, "Table");
  box->borders = gtk_check_button_new_with_mnemonic ("_Borders around every cell");
  gtk_check_button_set_active (GTK_CHECK_BUTTON (box->borders), w42_view_table_get_borders (view));
  gtk_grid_attach (GTK_GRID (grid), box->borders, 0, 0, 2, 1);
  box->header = gtk_check_button_new_with_mnemonic ("_Repeat the first row at the top of each page");
  gtk_check_button_set_active (GTK_CHECK_BUTTON (box->header), w42_view_table_get_header_rows (view) > 0);
  gtk_grid_attach (GTK_GRID (grid), box->header, 0, 1, 2, 1);

  grid = group (content, "This Row");
  {
    double unit = w42_settings_get_units () == W42_UNITS_CM ? 2.54 : 1.0;

    box->row_height = inches_row (grid, 0, 0, "Height at _least:",
                                  w42_view_table_get_row_height (view) / 1440.0 * unit);
  }

  grid = group (content, "This Cell");
  for (guint i = 0; i < G_N_ELEMENTS (SHADING_VALUES); i++)
    if (SHADING_VALUES[i] == now.shading)
      shading_index = i;
  box->shading = choice_row (grid, 0, 0, "_Shading:", SHADINGS, shading_index);
  {
    guint32 fill_rgb = 0;
    gboolean has_fill = w42_view_cell_get_fill (view, &fill_rgb);

    box->fill = choice_row (grid, 1, 0, "Bac_kground:", FILL_NAMES,
                            has_fill ? palette_index (fill_rgb) + 1 : 0);
  }
  {
    static const char *const names[4] = { "_Top", "Botto_m", "Le_ft", "Ri_ght" };
    static const int bits[4] = { W42_BORDER_TOP, W42_BORDER_BOTTOM, W42_BORDER_LEFT, W42_BORDER_RIGHT };
    GtkWidget *label = gtk_label_new ("Borders:");

    box->sides_before = w42_view_cell_get_borders (view);
    gtk_label_set_xalign (GTK_LABEL (label), 0.0);
    gtk_grid_attach (GTK_GRID (grid), label, 0, 2, 1, 1);
    for (int i = 0; i < 4; i++)
      {
        box->side[i] = gtk_check_button_new_with_mnemonic (names[i]);
        gtk_check_button_set_active (GTK_CHECK_BUTTON (box->side[i]), (box->sides_before & bits[i]) != 0);
        gtk_grid_attach (GTK_GRID (grid), box->side[i], 1 + i % 2, 2 + i / 2, 1, 1);
      }
  }
  {
    /* The line the cell's sides are drawn with: the cell's own, or the
     * table's where the cell has none. */
    W42BorderEdge edges[W42_N_EDGES];
    const W42BorderEdge *lead;

    box->have_cell = w42_view_cell_get_fmt (view, &box->cell_before);
    lead = &box->cell_before.edge[0];
    if ((box->cell_before.edge[0].width == 0 && box->cell_before.edge[0].style == 0) &&
        w42_view_table_get_edges (view, edges))
      lead = &edges[W42_EDGE_TOP];
    box->style = choice_row (grid, 4, 0, "Line st_yle:", BORDER_STYLES,
                             lead->style <= W42_BORDER_DOTTED ? lead->style : 0);
    box->width = choice_row (grid, 5, 0, "Line _width:", BORDER_WIDTHS,
                             width_index_for (W42_EDGE_WIDTH (lead)));
    box->colour = choice_row (grid, 6, 0, "Line _color:", PALETTE_NAMES, palette_index (lead->color));
    box->valign = choice_row (grid, 7, 0, "_Vertical alignment:", VALIGN_NAMES,
                              MIN (box->cell_before.cell_valign, W42_CELL_VALIGN_BOTTOM));
  }

  button_row (content, box->window, G_CALLBACK (on_table_props_ok), box);
  gtk_window_present (GTK_WINDOW (box->window));
}

/* ---------------------------------------------------------------------- */
/* Format > AutoFormat                                                     */
/* ---------------------------------------------------------------------- */

typedef struct {
  GtkWidget *window;
  W42View   *view;
  GtkWidget *headings;
  GtkWidget *lists;
  GtkWidget *quotes;
  GtkWidget *blanks;
} AutoFormatDialog;

static void
autoformat_dialog_free (gpointer data, GObject *where)
{
  (void) where;
  g_free (data);
}

static void
on_document_autoformat_ok (GtkButton *button, gpointer data)
{
  AutoFormatDialog *box = data;
  W42AutoFormat what;
  int changed;

  (void) button;
  what.headings = gtk_check_button_get_active (GTK_CHECK_BUTTON (box->headings));
  what.lists = gtk_check_button_get_active (GTK_CHECK_BUTTON (box->lists));
  what.quotes = gtk_check_button_get_active (GTK_CHECK_BUTTON (box->quotes));
  what.blanks = gtk_check_button_get_active (GTK_CHECK_BUTTON (box->blanks));

  w42_settings_set_bool ("autoformat-headings", what.headings);
  w42_settings_set_bool ("autoformat-lists", what.lists);
  w42_settings_set_bool ("autoformat-quotes", what.quotes);
  w42_settings_set_bool ("autoformat-blanks", what.blanks);

  changed = w42_view_autoformat (box->view, &what);
  gtk_window_destroy (GTK_WINDOW (box->window));

  /* Word 6 said what it had done and offered to look through it; this
   * says what it did, and Ctrl+Z takes the lot back. */
  {
    char *detail = changed > 0
      ? g_strdup_printf ("%d paragraph%s changed.  Undo takes the whole "
                         "thing back in one step.", changed, changed == 1 ? "" : "s")
      : g_strdup ("Nothing needed changing.");

    w42_message_show (gtk_window_get_transient_for (GTK_WINDOW (box->window)),
                      "AutoFormat", detail);
    g_free (detail);
  }
}

void
w42_autoformat_dialog_show (GtkWindow *parent, W42View *view)
{
  AutoFormatDialog *box;
  GtkWidget *content, *grid, *label;

  g_return_if_fail (W42_IS_VIEW (view));

  if (w42_view_get_document (view) == NULL)
    return;

  box = g_new0 (AutoFormatDialog, 1);
  box->view = view;
  box->window = dialog_shell (parent, "AutoFormat", &content, view);
  g_object_weak_ref (G_OBJECT (box->window), autoformat_dialog_free, box);

  label = gtk_label_new ("Word42 will look over the whole document and put "
                         "right what was typed as though on a typewriter.");
  gtk_label_set_xalign (GTK_LABEL (label), 0.0);
  gtk_label_set_wrap (GTK_LABEL (label), TRUE);
  gtk_widget_set_size_request (label, 330, -1);
  gtk_box_append (GTK_BOX (content), label);

  grid = group (content, "Apply");
  box->headings = gtk_check_button_new_with_mnemonic ("_Headings: short lines that stand alone");
  box->lists = gtk_check_button_new_with_mnemonic ("_Lists: lines that start with a dash or a number");
  box->quotes = gtk_check_button_new_with_mnemonic ("_Quotes and dashes as a printer sets them");
  box->blanks = gtk_check_button_new_with_mnemonic ("_Empty paragraphs: a run of them becomes one");
  gtk_check_button_set_active (GTK_CHECK_BUTTON (box->headings),
                               w42_settings_get_bool ("autoformat-headings", TRUE));
  gtk_check_button_set_active (GTK_CHECK_BUTTON (box->lists),
                               w42_settings_get_bool ("autoformat-lists", TRUE));
  gtk_check_button_set_active (GTK_CHECK_BUTTON (box->quotes),
                               w42_settings_get_bool ("autoformat-quotes", TRUE));
  gtk_check_button_set_active (GTK_CHECK_BUTTON (box->blanks),
                               w42_settings_get_bool ("autoformat-blanks", TRUE));
  gtk_grid_attach (GTK_GRID (grid), box->headings, 0, 0, 1, 1);
  gtk_grid_attach (GTK_GRID (grid), box->lists, 0, 1, 1, 1);
  gtk_grid_attach (GTK_GRID (grid), box->quotes, 0, 2, 1, 1);
  gtk_grid_attach (GTK_GRID (grid), box->blanks, 0, 3, 1, 1);

  button_row (content, box->window, G_CALLBACK (on_document_autoformat_ok), box);
  gtk_window_present (GTK_WINDOW (box->window));
}

/* ---------------------------------------------------------------------- */
/* Insert > Index Entry                                                    */
/* ---------------------------------------------------------------------- */

typedef struct {
  GtkWidget *window;
  W42View   *view;
  GtkWidget *term;
} IndexEntryBox;

static void
index_entry_free (gpointer data, GObject *where)
{
  (void) where;
  g_free (data);
}

static void
on_index_entry_ok (GtkButton *button, gpointer data)
{
  IndexEntryBox *box = data;
  const char *term = gtk_editable_get_text (GTK_EDITABLE (box->term));

  (void) button;
  w42_view_mark_index_entry (box->view, term);
  gtk_window_destroy (GTK_WINDOW (box->window));
}

void
w42_index_entry_dialog_show (GtkWindow *parent, W42View *view)
{
  IndexEntryBox *box;
  GtkWidget *content, *grid, *label;
  char *selected;

  g_return_if_fail (W42_IS_VIEW (view));

  if (w42_view_get_document (view) == NULL)
    return;

  selected = w42_view_get_selected_text (view);
  if (selected == NULL || *selected == '\0')
    {
      w42_message_show (parent, "Select the words to put in the index first.",
                        "Insert \342\226\270 Index Entry marks what is selected; "
                        "Insert \342\226\270 Index then gathers the marked words "
                        "and the pages they are on.");
      g_free (selected);
      return;
    }

  box = g_new0 (IndexEntryBox, 1);
  box->view = view;
  box->window = dialog_shell (parent, "Index Entry", &content, view);
  g_object_weak_ref (G_OBJECT (box->window), index_entry_free, box);

  grid = group (content, "Mark the selected words");
  label = gtk_label_new_with_mnemonic ("_File it under:");
  box->term = gtk_entry_new ();
  gtk_editable_set_text (GTK_EDITABLE (box->term), selected);
  gtk_entry_set_activates_default (GTK_ENTRY (box->term), TRUE);
  gtk_widget_set_size_request (box->term, 240, -1);
  gtk_label_set_xalign (GTK_LABEL (label), 0.0);
  gtk_label_set_mnemonic_widget (GTK_LABEL (label), box->term);
  gtk_grid_attach (GTK_GRID (grid), label, 0, 0, 1, 1);
  gtk_grid_attach (GTK_GRID (grid), box->term, 1, 0, 1, 1);

  label = gtk_label_new ("The words stay as they are and read as they did; "
                         "the index says which pages they are on.");
  gtk_label_set_xalign (GTK_LABEL (label), 0.0);
  gtk_label_set_wrap (GTK_LABEL (label), TRUE);
  gtk_widget_set_size_request (label, 320, -1);
  gtk_box_append (GTK_BOX (content), label);

  button_row (content, box->window, G_CALLBACK (on_index_entry_ok), box);
  gtk_window_present (GTK_WINDOW (box->window));
  gtk_widget_grab_focus (box->term);
  g_free (selected);
}

/* ---------------------------------------------------------------------- */
/* File > New from Template                                                */
/* ---------------------------------------------------------------------- */

typedef struct {
  GtkWidget *window;
  W42View   *view;
  GtkWidget *list;
  GtkWidget *hint;
  char     **files;         /* what is in the templates folder */
  int        n_built_in;
} TemplateBox;

static void
template_free (gpointer data, GObject *where)
{
  TemplateBox *box = data;

  (void) where;
  g_strfreev (box->files);
  g_free (box);
}

static void
template_chosen (GtkListBox *list, GtkListBoxRow *row, gpointer data)
{
  TemplateBox *box = data;
  int n = 0;
  const W42Template *templates = w42_templates (&n);
  int which;

  (void) list;
  if (row == NULL)
    return;
  which = gtk_list_box_row_get_index (row);
  if (which < n)
    gtk_label_set_text (GTK_LABEL (box->hint), templates[which].hint);
  else
    gtk_label_set_text (GTK_LABEL (box->hint),
                        "A document of your own, kept in the templates folder.");
}

static void
on_template_ok (GtkButton *button, gpointer data)
{
  TemplateBox *box = data;
  GtkListBoxRow *row = gtk_list_box_get_selected_row (GTK_LIST_BOX (box->list));
  GtkWindow *parent = gtk_window_get_transient_for (GTK_WINDOW (box->window));
  int which = row != NULL ? gtk_list_box_row_get_index (row) : 0;
  W42Document *doc;

  (void) button;

  doc = w42_window_new_document (parent);
  if (doc == NULL)
    return;

  if (which < box->n_built_in)
    {
      W42PageSetup page = *w42_document_page_setup (doc);

      w42_template_make (w42_document_pt (doc), &page, which);
      w42_document_set_page_setup (doc, &page);
      w42_document_set_modified (doc, which != 0);
      w42_document_touch (doc);
    }
  else
    {
      /* One of the user's own: it is opened and then forgotten, so that
       * saving asks where to put the new document rather than writing
       * over the template. */
      char *dir = w42_template_folder ();
      char *path = g_build_filename (dir, box->files[which - box->n_built_in], NULL);
      GFile *file = g_file_new_for_path (path);
      GError *error = NULL;

      if (w42_document_load (doc, file, &error))
        {
          w42_document_set_file (doc, NULL);
          w42_document_set_modified (doc, FALSE);
          w42_document_touch (doc);
        }
      else
        {
          w42_message_show (parent, "Word42 could not open that template.",
                            error != NULL ? error->message : NULL);
        }
      g_clear_error (&error);
      g_object_unref (file);
      g_free (path);
      g_free (dir);
    }

  gtk_window_destroy (GTK_WINDOW (box->window));
}

void
w42_template_dialog_show (GtkWindow *parent, W42View *view)
{
  TemplateBox *box;
  GtkWidget *content, *grid, *scroller;
  int n = 0;
  const W42Template *templates = w42_templates (&n);

  g_return_if_fail (W42_IS_VIEW (view));

  box = g_new0 (TemplateBox, 1);
  box->view = view;
  box->n_built_in = n;
  box->files = w42_template_files ();
  box->window = dialog_shell (parent, "New from Template", &content, view);
  g_object_weak_ref (G_OBJECT (box->window), template_free, box);

  grid = group (content, "Templates");
  box->list = gtk_list_box_new ();
  gtk_list_box_set_selection_mode (GTK_LIST_BOX (box->list), GTK_SELECTION_BROWSE);
  for (int i = 0; i < n; i++)
    {
      GtkWidget *label = gtk_label_new (templates[i].name);

      gtk_label_set_xalign (GTK_LABEL (label), 0.0);
      gtk_widget_set_margin_start (label, 6);
      gtk_list_box_append (GTK_LIST_BOX (box->list), label);
    }
  for (guint i = 0; box->files != NULL && box->files[i] != NULL; i++)
    {
      GtkWidget *label = gtk_label_new (box->files[i]);

      gtk_label_set_xalign (GTK_LABEL (label), 0.0);
      gtk_widget_set_margin_start (label, 6);
      gtk_list_box_append (GTK_LIST_BOX (box->list), label);
    }
  scroller = gtk_scrolled_window_new ();
  gtk_scrolled_window_set_policy (GTK_SCROLLED_WINDOW (scroller),
                                  GTK_POLICY_NEVER, GTK_POLICY_AUTOMATIC);
  gtk_scrolled_window_set_child (GTK_SCROLLED_WINDOW (scroller), box->list);
  gtk_widget_set_size_request (scroller, 280, 160);
  gtk_grid_attach (GTK_GRID (grid), scroller, 0, 0, 1, 1);

  box->hint = gtk_label_new ("");
  gtk_label_set_xalign (GTK_LABEL (box->hint), 0.0);
  gtk_label_set_wrap (GTK_LABEL (box->hint), TRUE);
  gtk_widget_set_size_request (box->hint, 280, -1);
  gtk_grid_attach (GTK_GRID (grid), box->hint, 0, 1, 1, 1);

  g_signal_connect (box->list, "row-selected", G_CALLBACK (template_chosen), box);
  gtk_list_box_select_row (GTK_LIST_BOX (box->list),
                           gtk_list_box_get_row_at_index (GTK_LIST_BOX (box->list), 1));

  button_row (content, box->window, G_CALLBACK (on_template_ok), box);
  gtk_window_present (GTK_WINDOW (box->window));
}

/* ---------------------------------------------------------------------- */
/* Tools > Envelopes and Labels                                            */
/* ---------------------------------------------------------------------- */

typedef struct {
  GtkWidget *window;
  W42View   *view;
  GtkWidget *what;              /* envelope or labels */
  GtkWidget *delivery;          /* a text view: an address has lines */
  GtkWidget *sender;
  GtkWidget *envelope_size;
  GtkWidget *label_sheet;
  GtkWidget *label_all;
} EnvelopeBox;

static const char * const ENVELOPE_OR_LABELS[] = { "Envelope", "Labels", NULL };

static void
envelope_free (gpointer data, GObject *where)
{
  (void) where;
  g_free (data);
}

/* Everything in a text view, as one string. */
static char *
text_view_text (GtkWidget *view)
{
  GtkTextBuffer *buffer = gtk_text_view_get_buffer (GTK_TEXT_VIEW (view));
  GtkTextIter start, end;

  gtk_text_buffer_get_bounds (buffer, &start, &end);
  return gtk_text_buffer_get_text (buffer, &start, &end, FALSE);
}

static void
on_envelope_ok (GtkButton *button, gpointer data)
{
  EnvelopeBox *box = data;
  gboolean labels = gtk_drop_down_get_selected (GTK_DROP_DOWN (box->what)) == 1;
  char *delivery = text_view_text (box->delivery);
  char *sender = text_view_text (box->sender);
  GtkWindow *parent = GTK_WINDOW (gtk_window_get_transient_for (GTK_WINDOW (box->window)));
  W42Document *doc;
  W42PageSetup page;

  (void) button;

  /* The envelope or the sheet is a document of its own, in a window of
   * its own: Word 6 offered to put it in front of the letter, but a
   * separate document is the honest thing when a section cannot have a
   * page size of its own yet. */
  doc = w42_window_new_document (parent);
  if (doc == NULL)
    {
      g_free (delivery);
      g_free (sender);
      return;
    }

  page = *w42_document_page_setup (doc);
  if (labels)
    w42_labels_make (w42_document_pt (doc), &page,
                     (int) gtk_drop_down_get_selected (GTK_DROP_DOWN (box->label_sheet)),
                     delivery,
                     gtk_check_button_get_active (GTK_CHECK_BUTTON (box->label_all)));
  else
    w42_envelope_make (w42_document_pt (doc), &page,
                       (int) gtk_drop_down_get_selected (GTK_DROP_DOWN (box->envelope_size)),
                       delivery, sender);
  w42_document_set_page_setup (doc, &page);
  w42_document_set_modified (doc, TRUE);
  w42_document_touch (doc);

  g_free (delivery);
  g_free (sender);
  gtk_window_destroy (GTK_WINDOW (box->window));
}

/* A framed text view, for an address of several lines. */
static GtkWidget *
address_box (GtkWidget *grid, int row, const char *label, GtkWidget **view)
{
  GtkWidget *text = gtk_label_new_with_mnemonic (label);
  GtkWidget *scroller = gtk_scrolled_window_new ();

  *view = gtk_text_view_new ();
  gtk_text_view_set_wrap_mode (GTK_TEXT_VIEW (*view), GTK_WRAP_WORD);
  gtk_label_set_xalign (GTK_LABEL (text), 0.0);
  gtk_label_set_yalign (GTK_LABEL (text), 0.0);
  gtk_label_set_mnemonic_widget (GTK_LABEL (text), *view);
  gtk_scrolled_window_set_policy (GTK_SCROLLED_WINDOW (scroller),
                                  GTK_POLICY_NEVER, GTK_POLICY_AUTOMATIC);
  gtk_scrolled_window_set_child (GTK_SCROLLED_WINDOW (scroller), *view);
  gtk_widget_set_size_request (scroller, 260, 72);
  gtk_grid_attach (GTK_GRID (grid), text, 0, row, 1, 1);
  gtk_grid_attach (GTK_GRID (grid), scroller, 1, row, 1, 1);
  return scroller;
}

void
w42_envelope_dialog_show (GtkWindow *parent, W42View *view)
{
  EnvelopeBox *box;
  GtkWidget *content, *grid, *label;
  int n_env = 0, n_lab = 0;
  const W42EnvelopeSize *envs = w42_envelope_sizes (&n_env);
  const W42LabelSheet *sheets = w42_label_sheets (&n_lab);
  const char **env_names, **sheet_names;

  g_return_if_fail (W42_IS_VIEW (view));

  if (w42_view_get_document (view) == NULL)
    return;

  box = g_new0 (EnvelopeBox, 1);
  box->view = view;
  box->window = dialog_shell (parent, "Envelopes and Labels", &content, view);
  g_object_weak_ref (G_OBJECT (box->window), envelope_free, box);

  grid = group (content, "What to make");
  box->what = choice_row (grid, 0, 0, "_Make:", ENVELOPE_OR_LABELS, 0);

  grid = group (content, "Addresses");
  address_box (grid, 0, "_Delivery address:", &box->delivery);
  address_box (grid, 1, "_Return address:", &box->sender);

  /* An address is often already in the document: the selection is the
   * likeliest one, as Word 6 took it. */
  {
    char *selected = w42_view_get_selected_text (view);

    if (selected != NULL && *selected != '\0')
      gtk_text_buffer_set_text (gtk_text_view_get_buffer (GTK_TEXT_VIEW (box->delivery)),
                                selected, -1);
    g_free (selected);
  }
  {
    char *me = w42_settings_get_string ("user-name", "");

    if (me != NULL && *me != '\0')
      gtk_text_buffer_set_text (gtk_text_view_get_buffer (GTK_TEXT_VIEW (box->sender)),
                                me, -1);
    g_free (me);
  }

  grid = group (content, "Sizes");
  env_names = g_new0 (const char *, n_env + 1);
  for (int i = 0; i < n_env; i++)
    env_names[i] = envs[i].name;
  box->envelope_size = choice_row (grid, 0, 0, "_Envelope:", env_names, 0);
  gtk_widget_set_size_request (box->envelope_size, 250, -1);
  g_free (env_names);

  sheet_names = g_new0 (const char *, n_lab + 1);
  for (int i = 0; i < n_lab; i++)
    sheet_names[i] = sheets[i].name;
  box->label_sheet = choice_row (grid, 1, 0, "_Label sheet:", sheet_names, 0);
  gtk_widget_set_size_request (box->label_sheet, 250, -1);
  g_free (sheet_names);

  box->label_all = gtk_check_button_new_with_mnemonic ("The same on _every label");
  gtk_check_button_set_active (GTK_CHECK_BUTTON (box->label_all), TRUE);
  gtk_grid_attach (GTK_GRID (grid), box->label_all, 0, 2, 2, 1);

  label = gtk_label_new ("The envelope or the sheet of labels is made as a "
                         "document of its own, in a new window.");
  gtk_label_set_xalign (GTK_LABEL (label), 0.0);
  gtk_label_set_wrap (GTK_LABEL (label), TRUE);
  gtk_widget_set_size_request (label, 360, -1);
  gtk_box_append (GTK_BOX (content), label);

  button_row (content, box->window, G_CALLBACK (on_envelope_ok), box);
  gtk_window_present (GTK_WINDOW (box->window));
}

/* ---------------------------------------------------------------------- */
/* Format > Background                                                     */
/* ---------------------------------------------------------------------- */

typedef struct {
  GtkWidget *window;
  W42View   *view;
  GtkWidget *colour;
  GtkWidget *sample;
  guint32    rgb;
  gboolean   none;
} BackgroundBox;

/* White paper and the fifteen colours the Font box offers. */
static const char * const PAGE_COLOURS[] = {
  "None (white paper)", "Light Gray", "Dark Gray", "Yellow", "Cyan", "Green",
  "Magenta", "Red", "Blue", "Dark Blue", "Dark Cyan", "Dark Green",
  "Dark Magenta", "Dark Red", "Dark Yellow", "Black", NULL
};
static const guint32 PAGE_COLOUR_VALUES[] = {
  0xFFFFFF, 0xC0C0C0, 0x808080, 0xFFFFC0, 0xC0FFFF, 0xC0FFC0,
  0xFFC0FF, 0xFFC0C0, 0xC0C0FF, 0x000080, 0x008080, 0x008000,
  0x800080, 0x800000, 0x808000, 0x000000
};

static void
background_free (gpointer data, GObject *where)
{
  (void) where;
  g_free (data);
}

/* The chosen colour, drawn as a page of it. */
static void
draw_background_sample (GtkDrawingArea *area, cairo_t *cr, int width, int height,
                        gpointer data)
{
  BackgroundBox *box = data;
  guint which = gtk_drop_down_get_selected (GTK_DROP_DOWN (box->colour));
  guint32 rgb = which < G_N_ELEMENTS (PAGE_COLOUR_VALUES)
                  ? PAGE_COLOUR_VALUES[which] : 0xFFFFFF;

  (void) area;
  cairo_set_source_rgb (cr, 0.86, 0.86, 0.86);
  cairo_paint (cr);
  cairo_set_source_rgb (cr, ((rgb >> 16) & 0xFF) / 255.0, ((rgb >> 8) & 0xFF) / 255.0,
                        (rgb & 0xFF) / 255.0);
  cairo_rectangle (cr, 8, 6, width - 16, height - 12);
  cairo_fill_preserve (cr);
  cairo_set_source_rgb (cr, 0.4, 0.4, 0.4);
  cairo_set_line_width (cr, 1.0);
  cairo_stroke (cr);

  /* Three lines of "text", so that the colour is judged behind text. */
  cairo_set_source_rgb (cr, 0.25, 0.25, 0.25);
  for (int i = 0; i < 3; i++)
    {
      cairo_rectangle (cr, 18, 18 + i * 10, width - 36 - (i == 2 ? 30 : 0), 3);
      cairo_fill (cr);
    }
}

static void
on_background_changed (GtkDropDown *drop, GParamSpec *spec, gpointer data)
{
  BackgroundBox *box = data;

  (void) drop; (void) spec;
  gtk_widget_queue_draw (box->sample);
}

static void
on_background_ok (GtkButton *button, gpointer data)
{
  BackgroundBox *box = data;
  W42Document *doc = w42_view_get_document (box->view);
  guint which = gtk_drop_down_get_selected (GTK_DROP_DOWN (box->colour));
  W42PageSetup page;

  (void) button;
  if (doc == NULL)
    return;

  page = *w42_document_page_setup (doc);
  if (which == 0)
    {
      page.has_background = 0;
      page.background = 0;
    }
  else if (which < G_N_ELEMENTS (PAGE_COLOUR_VALUES))
    {
      page.has_background = 1;
      page.background = PAGE_COLOUR_VALUES[which];
    }
  w42_document_set_page_setup (doc, &page);
  gtk_window_destroy (GTK_WINDOW (box->window));
}

void
w42_background_dialog_show (GtkWindow *parent, W42View *view)
{
  BackgroundBox *box;
  GtkWidget *content, *grid, *label;
  const W42PageSetup *page;
  guint selected = 0;

  g_return_if_fail (W42_IS_VIEW (view));

  if (w42_view_get_document (view) == NULL)
    return;

  box = g_new0 (BackgroundBox, 1);
  box->view = view;
  box->window = dialog_shell (parent, "Background", &content, view);
  g_object_weak_ref (G_OBJECT (box->window), background_free, box);

  page = w42_document_page_setup (w42_view_get_document (view));
  if (page != NULL && page->has_background)
    for (guint i = 1; i < G_N_ELEMENTS (PAGE_COLOUR_VALUES); i++)
      if (PAGE_COLOUR_VALUES[i] == (page->background & 0xFFFFFF))
        selected = i;

  grid = group (content, "The colour behind the page");
  box->colour = choice_row (grid, 0, 0, "_Color:", PAGE_COLOURS, selected);
  g_signal_connect (box->colour, "notify::selected",
                    G_CALLBACK (on_background_changed), box);

  box->sample = gtk_drawing_area_new ();
  gtk_widget_set_size_request (box->sample, 190, 58);
  gtk_drawing_area_set_draw_func (GTK_DRAWING_AREA (box->sample),
                                  draw_background_sample, box, NULL);
  gtk_grid_attach (GTK_GRID (grid), box->sample, 0, 1, 2, 1);

  label = gtk_label_new ("The colour is shown on the screen and in Print "
                         "Preview; printing leaves the paper as it is.");
  gtk_label_set_xalign (GTK_LABEL (label), 0.0);
  gtk_label_set_wrap (GTK_LABEL (label), TRUE);
  gtk_widget_set_size_request (label, 300, -1);
  gtk_box_append (GTK_BOX (content), label);

  button_row (content, box->window, G_CALLBACK (on_background_ok), box);
  gtk_window_present (GTK_WINDOW (box->window));
}

/* ---------------------------------------------------------------------- */
/* Edit > AutoText                                                         */
/* ---------------------------------------------------------------------- */

typedef struct {
  GtkWidget *window;
  W42View   *view;
  GtkWidget *name;
  GtkWidget *list;
  GtkWidget *preview;
  GtkWidget *add, *insert, *delete;
} AutoTextBox;

static void
autotext_free (gpointer data, GObject *where)
{
  (void) where;
  g_free (data);
}

static char *
autotext_chosen (AutoTextBox *box)
{
  GtkListBoxRow *row = gtk_list_box_get_selected_row (GTK_LIST_BOX (box->list));
  GtkWidget *label = row != NULL ? gtk_list_box_row_get_child (row) : NULL;

  return label != NULL ? g_strdup (gtk_label_get_text (GTK_LABEL (label))) : NULL;
}

static void
autotext_refill (AutoTextBox *box, const char *select)
{
  char **names = w42_autotext_names ();
  GtkWidget *row;
  int pick = -1;

  while ((row = gtk_widget_get_first_child (box->list)) != NULL)
    gtk_list_box_remove (GTK_LIST_BOX (box->list), row);

  for (guint i = 0; names != NULL && names[i] != NULL; i++)
    {
      GtkWidget *label = gtk_label_new (names[i]);

      gtk_label_set_xalign (GTK_LABEL (label), 0.0);
      gtk_widget_set_margin_start (label, 6);
      gtk_list_box_append (GTK_LIST_BOX (box->list), label);
      if (select != NULL && g_ascii_strcasecmp (names[i], select) == 0)
        pick = (int) i;
    }
  if (pick >= 0)
    gtk_list_box_select_row (GTK_LIST_BOX (box->list),
                             gtk_list_box_get_row_at_index (GTK_LIST_BOX (box->list), pick));
  g_strfreev (names);
}

/* What the chosen entry holds, shown small so that the right one is
 * picked without putting it in the document first. */
static void
autotext_selected (GtkListBox *list, GtkListBoxRow *row, gpointer data)
{
  AutoTextBox *box = data;
  char *name, *text;

  (void) list; (void) row;
  name = autotext_chosen (box);
  text = name != NULL ? w42_autotext_get (name) : NULL;

  if (name != NULL)
    gtk_editable_set_text (GTK_EDITABLE (box->name), name);
  if (text != NULL)
    {
      char *one_line = g_strdelimit (g_strdup (text), "\n\r", ' ');

      gtk_label_set_text (GTK_LABEL (box->preview), one_line);
      g_free (one_line);
    }
  else
    gtk_label_set_text (GTK_LABEL (box->preview), "");

  gtk_widget_set_sensitive (box->insert, text != NULL);
  gtk_widget_set_sensitive (box->delete, text != NULL);
  g_free (name);
  g_free (text);
}

static void
on_autotext_add (GtkButton *button, gpointer data)
{
  AutoTextBox *box = data;
  const char *name = gtk_editable_get_text (GTK_EDITABLE (box->name));
  char *text = w42_view_get_selected_text (box->view);

  (void) button;
  if (text == NULL || *text == '\0')
    {
      gtk_label_set_text (GTK_LABEL (box->preview),
                          "Select the text to keep, then Add.");
      g_free (text);
      return;
    }
  if (name == NULL || *name == '\0')
    {
      gtk_label_set_text (GTK_LABEL (box->preview), "Give the entry a name.");
      g_free (text);
      return;
    }

  w42_autotext_set (name, text);
  autotext_refill (box, name);
  g_free (text);
}

static void
on_autotext_insert (GtkButton *button, gpointer data)
{
  AutoTextBox *box = data;
  char *name = autotext_chosen (box);
  char *text = name != NULL ? w42_autotext_get (name) : NULL;

  (void) button;
  if (text != NULL)
    w42_view_insert_text (box->view, text);
  g_free (name);
  g_free (text);
  gtk_window_destroy (GTK_WINDOW (box->window));
}

static void
on_autotext_delete (GtkButton *button, gpointer data)
{
  AutoTextBox *box = data;
  char *name = autotext_chosen (box);

  (void) button;
  if (name != NULL)
    w42_autotext_remove (name);
  autotext_refill (box, NULL);
  gtk_label_set_text (GTK_LABEL (box->preview), "");
  gtk_widget_set_sensitive (box->insert, FALSE);
  gtk_widget_set_sensitive (box->delete, FALSE);
  g_free (name);
}

void
w42_autotext_dialog_show (GtkWindow *parent, W42View *view)
{
  AutoTextBox *box;
  GtkWidget *content, *grid, *label, *scroller, *buttons, *close;
  char *selected;

  g_return_if_fail (W42_IS_VIEW (view));

  if (w42_view_get_document (view) == NULL)
    return;

  box = g_new0 (AutoTextBox, 1);
  box->view = view;
  box->window = dialog_shell (parent, "AutoText", &content, view);
  g_object_weak_ref (G_OBJECT (box->window), autotext_free, box);

  grid = group (content, "Entries");

  label = gtk_label_new_with_mnemonic ("_Name:");
  gtk_label_set_xalign (GTK_LABEL (label), 0.0);
  box->name = gtk_entry_new ();
  gtk_entry_set_activates_default (GTK_ENTRY (box->name), TRUE);
  gtk_label_set_mnemonic_widget (GTK_LABEL (label), box->name);
  gtk_grid_attach (GTK_GRID (grid), label, 0, 0, 1, 1);
  gtk_grid_attach (GTK_GRID (grid), box->name, 1, 0, 1, 1);

  box->list = gtk_list_box_new ();
  gtk_list_box_set_selection_mode (GTK_LIST_BOX (box->list), GTK_SELECTION_BROWSE);
  scroller = gtk_scrolled_window_new ();
  gtk_scrolled_window_set_policy (GTK_SCROLLED_WINDOW (scroller),
                                  GTK_POLICY_NEVER, GTK_POLICY_AUTOMATIC);
  gtk_scrolled_window_set_child (GTK_SCROLLED_WINDOW (scroller), box->list);
  gtk_widget_set_size_request (scroller, 260, 130);
  gtk_grid_attach (GTK_GRID (grid), scroller, 0, 1, 2, 1);

  box->preview = gtk_label_new ("");
  gtk_label_set_xalign (GTK_LABEL (box->preview), 0.0);
  gtk_label_set_ellipsize (GTK_LABEL (box->preview), PANGO_ELLIPSIZE_END);
  gtk_widget_set_size_request (box->preview, 260, -1);
  gtk_grid_attach (GTK_GRID (grid), box->preview, 0, 2, 2, 1);

  buttons = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 8);
  box->add = gtk_button_new_with_mnemonic ("_Add");
  box->insert = gtk_button_new_with_mnemonic ("_Insert");
  box->delete = gtk_button_new_with_mnemonic ("_Delete");
  close = gtk_button_new_with_mnemonic ("_Close");
  gtk_widget_set_size_request (box->add, 84, 26);
  gtk_widget_set_size_request (box->insert, 84, 26);
  gtk_widget_set_size_request (box->delete, 84, 26);
  gtk_widget_set_size_request (close, 84, 26);
  gtk_box_append (GTK_BOX (buttons), box->add);
  gtk_box_append (GTK_BOX (buttons), box->insert);
  gtk_box_append (GTK_BOX (buttons), box->delete);
  gtk_box_append (GTK_BOX (buttons), close);
  gtk_widget_set_halign (buttons, GTK_ALIGN_END);
  gtk_box_append (GTK_BOX (content), buttons);

  g_signal_connect (box->add, "clicked", G_CALLBACK (on_autotext_add), box);
  g_signal_connect (box->insert, "clicked", G_CALLBACK (on_autotext_insert), box);
  g_signal_connect (box->delete, "clicked", G_CALLBACK (on_autotext_delete), box);
  g_signal_connect_swapped (close, "clicked", G_CALLBACK (gtk_window_destroy), box->window);
  g_signal_connect (box->list, "row-selected", G_CALLBACK (autotext_selected), box);

  autotext_refill (box, NULL);

  /* With text selected the box is there to keep it: the name is filled
   * in from its first words, as Word 6 filled it in. */
  selected = w42_view_get_selected_text (view);
  if (selected != NULL && *selected != '\0')
    {
      char *suggestion = w42_autotext_suggest_name (selected);

      if (suggestion != NULL)
        gtk_editable_set_text (GTK_EDITABLE (box->name), suggestion);
      gtk_label_set_text (GTK_LABEL (box->preview),
                          "Add keeps the selected text under that name.");
      gtk_window_set_default_widget (GTK_WINDOW (box->window), box->add);
      g_free (suggestion);
    }
  else
    {
      gtk_widget_set_sensitive (box->add, FALSE);
      gtk_window_set_default_widget (GTK_WINDOW (box->window), box->insert);
    }
  g_free (selected);

  gtk_window_present (GTK_WINDOW (box->window));
}

/* ---------------------------------------------------------------------- */
/* Tools > Word Count                                                      */
/* ---------------------------------------------------------------------- */

typedef struct {
  GtkWidget *window;
  W42View   *view;
  GtkWidget *notes;             /* the "include the notes" switch */
  GtkWidget *value[6];          /* pages, words, characters, no spaces,
                                 * paragraphs, lines -- of the document */
  GtkWidget *selected[6];       /* and of the selection, when there is one */
  gboolean   has_selection;
} CountBox;

static const char * const COUNT_NAMES[6] = {
  "Pages", "Words", "Characters", "Characters (no spaces)", "Paragraphs", "Lines"
};

static void
count_free (gpointer data, GObject *where)
{
  (void) where;
  g_free (data);
}

/* How many lines the layout has laid out, notes and all. */
static gsize
count_lines (W42View *view)
{
  W42Layout *layout = w42_view_get_layout (view);
  const GArray *lines = layout != NULL ? w42_layout_lines (layout) : NULL;

  return lines != NULL ? lines->len : 0;
}

static void
count_fill (CountBox *box)
{
  W42PieceTable *pt = w42_view_get_document (box->view) != NULL
                        ? w42_document_pt (w42_view_get_document (box->view)) : NULL;
  gboolean with_notes = gtk_check_button_get_active (GTK_CHECK_BUTTON (box->notes));
  W42Stats doc;
  gsize counts[6];

  if (pt == NULL)
    return;

  w42_pt_statistics (pt, with_notes, &doc);
  counts[0] = (gsize) MAX (w42_layout_n_pages (w42_view_get_layout (box->view)), 1);
  counts[1] = doc.words;
  counts[2] = doc.characters;
  counts[3] = doc.characters_no_spaces;
  counts[4] = doc.paragraphs;
  counts[5] = count_lines (box->view);
  for (int i = 0; i < 6; i++)
    {
      char *text = g_strdup_printf ("%" G_GSIZE_FORMAT, counts[i]);

      gtk_label_set_text (GTK_LABEL (box->value[i]), text);
      g_free (text);
    }

  if (box->has_selection)
    {
      W42Stats sel;
      gsize start, end;

      w42_view_get_selection_bounds (box->view, &start, &end);
      w42_pt_statistics_range (pt, start, end, &sel);
      for (int i = 0; i < 6; i++)
        {
          gsize n = i == 1 ? sel.words : i == 2 ? sel.characters
                  : i == 3 ? sel.characters_no_spaces : i == 4 ? sel.paragraphs : 0;
          char *text = i == 0 || i == 5 ? g_strdup ("--")
                                        : g_strdup_printf ("%" G_GSIZE_FORMAT, n);

          gtk_label_set_text (GTK_LABEL (box->selected[i]), text);
          g_free (text);
        }
    }
}

static void
on_count_notes (GtkCheckButton *button, gpointer data)
{
  (void) button;
  count_fill (data);
}

void
w42_word_count_dialog_show (GtkWindow *parent, W42View *view)
{
  CountBox *box;
  GtkWidget *content, *grid, *close;

  g_return_if_fail (W42_IS_VIEW (view));

  if (w42_view_get_document (view) == NULL)
    return;

  box = g_new0 (CountBox, 1);
  box->view = view;
  box->has_selection = w42_view_has_selection (view);
  box->window = dialog_shell (parent, "Word Count", &content, view);
  g_object_weak_ref (G_OBJECT (box->window), count_free, box);

  grid = group (content, box->has_selection ? "Counts" : "Statistics");
  if (box->has_selection)
    {
      GtkWidget *a = gtk_label_new ("Document");
      GtkWidget *b = gtk_label_new ("Selection");

      gtk_label_set_xalign (GTK_LABEL (a), 1.0);
      gtk_label_set_xalign (GTK_LABEL (b), 1.0);
      gtk_grid_attach (GTK_GRID (grid), a, 1, 0, 1, 1);
      gtk_grid_attach (GTK_GRID (grid), b, 2, 0, 1, 1);
    }
  for (int i = 0; i < 6; i++)
    {
      GtkWidget *name = gtk_label_new (COUNT_NAMES[i]);

      gtk_label_set_xalign (GTK_LABEL (name), 0.0);
      box->value[i] = gtk_label_new ("0");
      gtk_label_set_xalign (GTK_LABEL (box->value[i]), 1.0);
      gtk_widget_set_size_request (box->value[i], 70, -1);
      gtk_grid_attach (GTK_GRID (grid), name, 0, i + 1, 1, 1);
      gtk_grid_attach (GTK_GRID (grid), box->value[i], 1, i + 1, 1, 1);
      if (box->has_selection)
        {
          box->selected[i] = gtk_label_new ("0");
          gtk_label_set_xalign (GTK_LABEL (box->selected[i]), 1.0);
          gtk_widget_set_size_request (box->selected[i], 70, -1);
          gtk_grid_attach (GTK_GRID (grid), box->selected[i], 2, i + 1, 1, 1);
        }
    }

  box->notes = gtk_check_button_new_with_mnemonic ("Include _footnotes and endnotes");
  gtk_check_button_set_active (GTK_CHECK_BUTTON (box->notes), TRUE);
  g_signal_connect (box->notes, "toggled", G_CALLBACK (on_count_notes), box);
  gtk_box_append (GTK_BOX (content), box->notes);

  count_fill (box);

  /* One button: there is nothing here to undo or apply. */
  close = gtk_button_new_with_mnemonic ("_Close");
  gtk_widget_set_halign (close, GTK_ALIGN_END);
  gtk_widget_set_size_request (close, 92, 26);
  g_signal_connect_swapped (close, "clicked", G_CALLBACK (gtk_window_destroy), box->window);
  gtk_box_append (GTK_BOX (content), close);
  gtk_window_set_default_widget (GTK_WINDOW (box->window), close);

  gtk_window_present (GTK_WINDOW (box->window));
}

/* ---------------------------------------------------------------------- */
/* Tools > Language                                                        */
/* ---------------------------------------------------------------------- */

typedef struct {
  GtkWidget *window;
  W42View   *view;
  W42Spell  *spell;
  GtkWidget *list;
  GtkWidget *note;
  int        chosen;        /* -1: the document's own language */
} LanguageBox;

static void
language_free (gpointer data, GObject *where)
{
  (void) where;
  g_free (data);
}

static void
language_chosen (GtkListBox *list, GtkListBoxRow *row, gpointer data)
{
  LanguageBox *box = data;
  int n = 0;
  const W42Language *langs = w42_languages (&n);

  (void) list;
  if (row == NULL)
    return;
  box->chosen = gtk_list_box_row_get_index (row) - 1;    /* row 0 is "own" */
  if (box->chosen < 0)
    gtk_label_set_text (GTK_LABEL (box->note),
                        "The text is checked with the document's own dictionary.");
  else if (g_strcmp0 (langs[box->chosen].tag, W42_LANG_NONE) == 0)
    gtk_label_set_text (GTK_LABEL (box->note),
                        "The text is not checked at all.");
  else if (box->spell != NULL &&
           w42_spell_has_language (box->spell, langs[box->chosen].tag))
    gtk_label_set_text (GTK_LABEL (box->note), "A dictionary for this is installed.");
  else
    gtk_label_set_text (GTK_LABEL (box->note),
                        "No dictionary for this is installed: the words are not checked.");
}

static void
on_language_ok (GtkButton *button, gpointer data)
{
  LanguageBox *box = data;
  int n = 0;
  const W42Language *langs = w42_languages (&n);
  W42CharFmt want;

  (void) button;
  memset (&want, 0, sizeof want);
  want.lang = box->chosen >= 0 && box->chosen < n
                ? g_intern_static_string (langs[box->chosen].tag) : NULL;
  w42_view_apply_char_fmt (box->view, W42_CHAR_LANG, &want);
  gtk_window_destroy (GTK_WINDOW (box->window));
}

void
w42_language_dialog_show (GtkWindow *parent, W42View *view, W42Spell *spell)
{
  LanguageBox *box;
  GtkWidget *content, *grid, *scroller, *label;
  int n = 0;
  const W42Language *langs = w42_languages (&n);
  W42CharFmt now;
  int selected = 0;

  g_return_if_fail (W42_IS_VIEW (view));

  if (w42_view_get_document (view) == NULL)
    return;

  box = g_new0 (LanguageBox, 1);
  box->view = view;
  box->spell = spell;
  box->chosen = -1;
  box->window = dialog_shell (parent, "Language", &content, view);
  g_object_weak_ref (G_OBJECT (box->window), language_free, box);

  grid = group (content, "Mark the selected text as");

  box->list = gtk_list_box_new ();
  gtk_list_box_set_selection_mode (GTK_LIST_BOX (box->list), GTK_SELECTION_BROWSE);
  {
    GtkWidget *own = gtk_label_new ("(the document's own language)");

    gtk_label_set_xalign (GTK_LABEL (own), 0.0);
    gtk_widget_set_margin_start (own, 6);
    gtk_list_box_append (GTK_LIST_BOX (box->list), own);
  }
  w42_view_get_char_fmt (view, &now);
  for (int i = 0; i < n; i++)
    {
      /* A tick for the languages there is a dictionary for, as the
       * classic box marked them. */
      gboolean have = spell != NULL && w42_spell_has_language (spell, langs[i].tag);
      char *text = g_strdup_printf ("%s%s", have ? "\342\234\223 " : "   ", langs[i].name);
      GtkWidget *item = gtk_label_new (text);

      gtk_label_set_xalign (GTK_LABEL (item), 0.0);
      gtk_widget_set_margin_start (item, 6);
      gtk_list_box_append (GTK_LIST_BOX (box->list), item);
      if (now.lang != NULL && g_strcmp0 (now.lang, langs[i].tag) == 0)
        selected = i + 1;
      g_free (text);
    }
  scroller = gtk_scrolled_window_new ();
  gtk_scrolled_window_set_policy (GTK_SCROLLED_WINDOW (scroller),
                                  GTK_POLICY_NEVER, GTK_POLICY_AUTOMATIC);
  gtk_scrolled_window_set_child (GTK_SCROLLED_WINDOW (scroller), box->list);
  gtk_widget_set_size_request (scroller, 280, 190);
  gtk_grid_attach (GTK_GRID (grid), scroller, 0, 0, 1, 1);

  box->note = gtk_label_new ("");
  gtk_label_set_xalign (GTK_LABEL (box->note), 0.0);
  gtk_label_set_wrap (GTK_LABEL (box->note), TRUE);
  gtk_widget_set_size_request (box->note, 280, -1);
  gtk_grid_attach (GTK_GRID (grid), box->note, 0, 1, 1, 1);

  {
    char *text = spell != NULL && w42_spell_language (spell) != NULL
                   ? g_strdup_printf ("The document's own dictionary is %s.",
                                      w42_spell_language (spell))
                   : g_strdup ("No dictionary was found on this machine.");

    label = gtk_label_new (text);
    g_free (text);
  }
  gtk_label_set_xalign (GTK_LABEL (label), 0.0);
  gtk_box_append (GTK_BOX (content), label);

  g_signal_connect (box->list, "row-selected", G_CALLBACK (language_chosen), box);
  gtk_list_box_select_row (GTK_LIST_BOX (box->list),
                           gtk_list_box_get_row_at_index (GTK_LIST_BOX (box->list), selected));

  button_row (content, box->window, G_CALLBACK (on_language_ok), box);
  gtk_window_present (GTK_WINDOW (box->window));
}

/* ---------------------------------------------------------------------- */
/* Table > Table AutoFormat                                                */
/* ---------------------------------------------------------------------- */

typedef struct {
  GtkWidget *window;
  W42View   *view;
  GtkWidget *list;
  GtkWidget *preview;
  GtkWidget *hint;
  GtkWidget *heading;
  GtkWidget *first_column;
  int        chosen;
} AutoFormatBox;

static void
autoformat_free (gpointer data, GObject *where)
{
  (void) where;
  g_free (data);
}

/* The look drawn small: four rows of three cells, the text of them a bar
 * apiece, so that the rules and the shading are what the eye reads. */
static void
draw_autoformat (GtkDrawingArea *area, cairo_t *cr, int width, int height,
                 gpointer data)
{
  const AutoFormatBox *box = data;
  int n = 0;
  const W42TableFormat *formats = w42_table_formats (&n);
  const W42TableFormat *f = &formats[CLAMP (box->chosen, 0, n - 1)];
  const int rows = 4, cols = 3;
  gboolean heading = gtk_check_button_get_active (GTK_CHECK_BUTTON (box->heading));
  gboolean first_col = gtk_check_button_get_active (GTK_CHECK_BUTTON (box->first_column));
  double m = 8.0;
  double w = (width - 2 * m) / cols, h = (height - 2 * m) / rows;

  (void) area;
  cairo_set_source_rgb (cr, 1.0, 1.0, 1.0);
  cairo_paint (cr);
  cairo_set_line_width (cr, 1.0);

  for (int r = 0; r < rows; r++)
    for (int c = 0; c < cols; c++)
      {
        double x = m + c * w, y = m + r * h;
        gboolean head = heading && r == 0;
        int shade = head ? f->head_shading
                         : (f->band_shading > 0 && (r % 2) == 1 ? f->band_shading : 0);
        gboolean bold = (head && f->head_bold) ||
                        (first_col && c == 0 && f->first_col_bold);

        if (shade > 0)
          {
            double g = 1.0 - shade / 100.0;

            cairo_set_source_rgb (cr, g, g, g);
            cairo_rectangle (cr, x, y, w, h);
            cairo_fill (cr);
          }

        /* The text: a bar, darker and thicker where it would be bold. */
        cairo_set_source_rgb (cr, bold ? 0.1 : 0.45, bold ? 0.1 : 0.45, bold ? 0.1 : 0.45);
        cairo_rectangle (cr, x + 4, y + h / 2 - (bold ? 2 : 1), w - 10, bold ? 4 : 2);
        cairo_fill (cr);

        cairo_set_source_rgb (cr, 0.0, 0.0, 0.0);
        if (f->rules == W42_TF_RULES_GRID)
          {
            cairo_rectangle (cr, x + 0.5, y + 0.5, w, h);
            cairo_stroke (cr);
          }
        else if (f->rules == W42_TF_RULES_BOX)
          {
            if (r == 0) { cairo_move_to (cr, x, y + 0.5); cairo_line_to (cr, x + w, y + 0.5); }
            if (r == rows - 1) { cairo_move_to (cr, x, y + h + 0.5); cairo_line_to (cr, x + w, y + h + 0.5); }
            if (heading && r == 0) { cairo_move_to (cr, x, y + h + 0.5); cairo_line_to (cr, x + w, y + h + 0.5); }
            if (c == 0) { cairo_move_to (cr, x + 0.5, y); cairo_line_to (cr, x + 0.5, y + h); }
            if (c == cols - 1) { cairo_move_to (cr, x + w + 0.5, y); cairo_line_to (cr, x + w + 0.5, y + h); }
            cairo_stroke (cr);
          }
      }
}

static void
autoformat_chosen (GtkListBox *list, GtkListBoxRow *row, gpointer data)
{
  AutoFormatBox *box = data;
  int n = 0;
  const W42TableFormat *formats = w42_table_formats (&n);

  (void) list;
  if (row == NULL)
    return;
  box->chosen = CLAMP (gtk_list_box_row_get_index (row), 0, n - 1);
  gtk_label_set_text (GTK_LABEL (box->hint), formats[box->chosen].hint);
  gtk_widget_queue_draw (box->preview);
}

static void
autoformat_switched (GtkCheckButton *button, gpointer data)
{
  AutoFormatBox *box = data;

  (void) button;
  gtk_widget_queue_draw (box->preview);
}

static void
on_autoformat_ok (GtkButton *button, gpointer data)
{
  AutoFormatBox *box = data;
  int n = 0;
  const W42TableFormat *formats = w42_table_formats (&n);

  (void) button;
  w42_view_table_autoformat (box->view, &formats[CLAMP (box->chosen, 0, n - 1)],
                             gtk_check_button_get_active (GTK_CHECK_BUTTON (box->heading)),
                             gtk_check_button_get_active (GTK_CHECK_BUTTON (box->first_column)));
  gtk_window_destroy (GTK_WINDOW (box->window));
}

void
w42_table_autoformat_dialog_show (GtkWindow *parent, W42View *view)
{
  AutoFormatBox *box;
  GtkWidget *content, *grid, *scroller, *label;
  int n = 0;
  const W42TableFormat *formats = w42_table_formats (&n);

  g_return_if_fail (W42_IS_VIEW (view));

  if (w42_view_get_document (view) == NULL)
    return;

  box = g_new0 (AutoFormatBox, 1);
  box->view = view;
  box->window = dialog_shell (parent, "Table AutoFormat", &content, view);
  g_object_weak_ref (G_OBJECT (box->window), autoformat_free, box);

  grid = group (content, "Formats");

  box->list = gtk_list_box_new ();
  gtk_list_box_set_selection_mode (GTK_LIST_BOX (box->list), GTK_SELECTION_BROWSE);
  for (int i = 0; i < n; i++)
    {
      GtkWidget *item = gtk_label_new (formats[i].name);

      gtk_label_set_xalign (GTK_LABEL (item), 0.0);
      gtk_widget_set_margin_start (item, 6);
      gtk_widget_set_margin_end (item, 6);
      gtk_widget_set_margin_top (item, 2);
      gtk_widget_set_margin_bottom (item, 2);
      gtk_list_box_append (GTK_LIST_BOX (box->list), item);
    }
  scroller = gtk_scrolled_window_new ();
  gtk_scrolled_window_set_policy (GTK_SCROLLED_WINDOW (scroller),
                                  GTK_POLICY_NEVER, GTK_POLICY_AUTOMATIC);
  gtk_scrolled_window_set_child (GTK_SCROLLED_WINDOW (scroller), box->list);
  gtk_widget_set_size_request (scroller, 150, 130);
  gtk_grid_attach (GTK_GRID (grid), scroller, 0, 0, 1, 1);

  box->preview = gtk_drawing_area_new ();
  gtk_widget_set_size_request (box->preview, 170, 130);
  gtk_grid_attach (GTK_GRID (grid), box->preview, 1, 0, 1, 1);

  box->hint = gtk_label_new ("");
  gtk_label_set_xalign (GTK_LABEL (box->hint), 0.0);
  gtk_grid_attach (GTK_GRID (grid), box->hint, 0, 1, 2, 1);

  grid = group (content, "Apply");
  box->heading = gtk_check_button_new_with_mnemonic ("_Heading row");
  box->first_column = gtk_check_button_new_with_mnemonic ("First _column");
  gtk_check_button_set_active (GTK_CHECK_BUTTON (box->heading), TRUE);
  gtk_check_button_set_active (GTK_CHECK_BUTTON (box->first_column), TRUE);
  gtk_grid_attach (GTK_GRID (grid), box->heading, 0, 0, 1, 1);
  gtk_grid_attach (GTK_GRID (grid), box->first_column, 1, 0, 1, 1);
  g_signal_connect (box->heading, "toggled", G_CALLBACK (autoformat_switched), box);
  g_signal_connect (box->first_column, "toggled", G_CALLBACK (autoformat_switched), box);

  label = gtk_label_new ("The look goes on the table the caret is in.");
  gtk_label_set_xalign (GTK_LABEL (label), 0.0);
  gtk_box_append (GTK_BOX (content), label);

  gtk_drawing_area_set_draw_func (GTK_DRAWING_AREA (box->preview),
                                  draw_autoformat, box, NULL);
  g_signal_connect (box->list, "row-selected", G_CALLBACK (autoformat_chosen), box);
  gtk_list_box_select_row (GTK_LIST_BOX (box->list),
                           gtk_list_box_get_row_at_index (GTK_LIST_BOX (box->list), 1));

  button_row (content, box->window, G_CALLBACK (on_autoformat_ok), box);
  gtk_window_present (GTK_WINDOW (box->window));
}

/* ---------------------------------------------------------------------- */
/* Insert > Field                                                          */
/* ---------------------------------------------------------------------- */

typedef struct {
  GtkWidget *window;
  W42View   *view;
  GtkWidget *kind;
} FieldBox;

static const char * const FIELD_NAMES[] = {
  "Page number", "Number of pages", "Date", "Time", "File name", "Word count", NULL
};
static const char * const FIELD_CODES[] = {
  "PAGE", "NUMPAGES", "DATE", "TIME", "FILENAME", "NUMWORDS"
};

static void
on_field_ok (GtkButton *button, gpointer data)
{
  FieldBox *box = data;
  guint which = gtk_drop_down_get_selected (GTK_DROP_DOWN (box->kind));

  (void) button;
  w42_view_insert_field (box->view, FIELD_CODES[MIN (which, G_N_ELEMENTS (FIELD_CODES) - 1)]);
  gtk_window_destroy (GTK_WINDOW (box->window));
}

void
w42_field_dialog_show (GtkWindow *parent, W42View *view)
{
  FieldBox *box;
  GtkWidget *content, *grid, *label;

  g_return_if_fail (W42_IS_VIEW (view));

  if (w42_view_get_document (view) == NULL)
    return;

  box = g_new0 (FieldBox, 1);
  box->view = view;
  box->window = dialog_shell (parent, "Field", &content, view);
  g_object_weak_ref (G_OBJECT (box->window), hf_free, box);

  grid = group (content, "Field");
  box->kind = choice_row (grid, 0, 0, "_Insert:", FIELD_NAMES, 0);
  label = gtk_label_new ("A field shows its result, shaded grey. F9 updates every field;\n"
                         "printing and exporting update them too.");
  gtk_label_set_xalign (GTK_LABEL (label), 0.0);
  gtk_widget_add_css_class (label, "w42-dialog-status");
  gtk_grid_attach (GTK_GRID (grid), label, 0, 1, 2, 1);

  button_row (content, box->window, G_CALLBACK (on_field_ok), box);
  gtk_window_present (GTK_WINDOW (box->window));
}

/* ---------------------------------------------------------------------- */
/* Format > Picture                                                        */
/* ---------------------------------------------------------------------- */

typedef struct {
  W42View   *view;
  GtkWidget *window;
  GtkWidget *width, *height, *wrap;
} PictureBox;

static void
picture_ok (GtkButton *button, gpointer data)
{
  PictureBox *box = data;
  static const W42Wrap wraps[] = { W42_WRAP_INLINE, W42_WRAP_LEFT, W42_WRAP_RIGHT,
                                   W42_WRAP_TOP_BOTTOM, W42_WRAP_FRONT, W42_WRAP_BEHIND };
  guint sel = gtk_drop_down_get_selected (GTK_DROP_DOWN (box->wrap));

  (void) button;
  w42_view_set_picture (box->view,
                        MAX (spin_twips (box->width), 15),
                        MAX (spin_twips (box->height), 15),
                        wraps[MIN (sel, G_N_ELEMENTS (wraps) - 1)]);
  gtk_window_close (GTK_WINDOW (box->window));
}

void
w42_picture_dialog_show (GtkWindow *parent, W42View *view)
{
  static const char *const wraps[] = {
    "In line with text", "Left, text to the right", "Right, text to the left",
    "Top and bottom", "In front of text", "Behind text", NULL
  };
  PictureBox *box;
  GtkWidget *content, *grid;
  int width = 0, height = 0;
  W42Wrap wrap = W42_WRAP_INLINE;
  gboolean cm = w42_settings_get_units () == W42_UNITS_CM;
  double unit = cm ? 2.54 : 1.0;

  if (!w42_view_get_picture (view, &width, &height, &wrap))
    {
      w42_message_show (parent,
                        "Select a picture first: click it once, so that its "
                        "handles show.", NULL);
      return;
    }

  box = g_new0 (PictureBox, 1);
  box->view = view;
  box->window = dialog_shell (parent, "Picture", &content, view);
  g_object_set_data_full (G_OBJECT (box->window), "w42-box", box, g_free);

  grid = group (content, "Size");
  box->width = inches_row (grid, 0, 0, "_Width:", width / 1440.0 * unit);
  box->height = inches_row (grid, 1, 0, "_Height:", height / 1440.0 * unit);

  grid = group (content, "Text wrapping");
  box->wrap = choice_row (grid, 0, 0, "_Position:", wraps, MIN ((guint) wrap, 5));

  button_row (content, box->window, G_CALLBACK (picture_ok), box);
  gtk_window_present (GTK_WINDOW (box->window));
}

/* ---------------------------------------------------------------------- */
/* Format > Drop Cap, Format > Frame                                       */
/* ---------------------------------------------------------------------- */

typedef struct {
  W42View   *view;
  GtkWidget *window;
  GtkWidget *position, *lines, *width;
} FrameBox;

static void
on_drop_position (GtkDropDown *drop, GParamSpec *pspec, gpointer data)
{
  FrameBox *box = data;

  (void) pspec;
  gtk_widget_set_sensitive (box->lines, gtk_drop_down_get_selected (drop) == 1);
}

static void
drop_cap_ok (GtkButton *button, gpointer data)
{
  FrameBox *box = data;
  guint pos = gtk_drop_down_get_selected (GTK_DROP_DOWN (box->position));

  (void) button;
  w42_view_set_drop_cap (box->view,
                         pos == 0 ? 0 : (int) gtk_spin_button_get_value (GTK_SPIN_BUTTON (box->lines)));
  gtk_window_close (GTK_WINDOW (box->window));
}

void
w42_drop_cap_dialog_show (GtkWindow *parent, W42View *view)
{
  static const char *const positions[] = { "None", "Dropped", NULL };
  FrameBox *box = g_new0 (FrameBox, 1);
  GtkWidget *content, *grid, *label;
  W42ParaFmt now;

  w42_view_get_para_fmt (view, &now);
  box->view = view;
  box->window = dialog_shell (parent, "Drop Cap", &content, view);
  g_object_set_data_full (G_OBJECT (box->window), "w42-box", box, g_free);

  grid = group (content, "Position");
  box->position = choice_row (grid, 0, 0, "_Position:", positions, now.drop_cap > 0 ? 1 : 0);
  label = gtk_label_new_with_mnemonic ("_Lines to drop:");
  box->lines = gtk_spin_button_new_with_range (1, 10, 1);
  gtk_spin_button_set_value (GTK_SPIN_BUTTON (box->lines), now.drop_cap > 0 ? now.drop_cap : 3);
  gtk_spin_button_set_activates_default (GTK_SPIN_BUTTON (box->lines), TRUE);
  gtk_label_set_xalign (GTK_LABEL (label), 0.0);
  gtk_label_set_mnemonic_widget (GTK_LABEL (label), box->lines);
  gtk_grid_attach (GTK_GRID (grid), label, 0, 1, 1, 1);
  gtk_grid_attach (GTK_GRID (grid), box->lines, 1, 1, 1, 1);
  gtk_widget_set_sensitive (box->lines, now.drop_cap > 0);
  g_signal_connect (box->position, "notify::selected", G_CALLBACK (on_drop_position), box);

  button_row (content, box->window, G_CALLBACK (drop_cap_ok), box);
  gtk_window_present (GTK_WINDOW (box->window));
}

static void
frame_ok (GtkButton *button, gpointer data)
{
  FrameBox *box = data;
  guint pos = gtk_drop_down_get_selected (GTK_DROP_DOWN (box->position));

  (void) button;
  w42_view_set_frame (box->view, (int) MIN (pos, 2), spin_twips (box->width));
  gtk_window_close (GTK_WINDOW (box->window));
}

void
w42_frame_dialog_show (GtkWindow *parent, W42View *view)
{
  static const char *const positions[] = { "None", "Left, text to the right", "Right, text to the left", NULL };
  FrameBox *box = g_new0 (FrameBox, 1);
  GtkWidget *content, *grid;
  W42ParaFmt now;
  double unit = w42_settings_get_units () == W42_UNITS_CM ? 2.54 : 1.0;

  w42_view_get_para_fmt (view, &now);
  box->view = view;
  box->window = dialog_shell (parent, "Frame", &content, view);
  g_object_set_data_full (G_OBJECT (box->window), "w42-box", box, g_free);

  grid = group (content, "Frame");
  box->position = choice_row (grid, 0, 0, "_Position:", positions, MIN (now.frame_side, 2));
  box->width = inches_row (grid, 1, 0, "_Width:", (now.frame_width > 0 ? now.frame_width : 2880) / 1440.0 * unit);

  button_row (content, box->window, G_CALLBACK (frame_ok), box);
  gtk_window_present (GTK_WINDOW (box->window));
}

/* ---------------------------------------------------------------------- */
/* File > Summary Info                                                     */
/* ---------------------------------------------------------------------- */

typedef struct {
  W42View   *view;
  GtkWidget *window;
  GtkWidget *field[5];      /* title, subject, author, keywords, comments */
} SummaryBox;

static void
summary_ok (GtkButton *button, gpointer data)
{
  SummaryBox *box = data;
  W42Document *doc = w42_view_get_document (box->view);
  W42DocInfo info;
  const W42DocInfo *had;
  gboolean changed = FALSE;

  (void) button;
  memset (&info, 0, sizeof info);
  info.title    = gtk_editable_get_text (GTK_EDITABLE (box->field[0]));
  info.subject  = gtk_editable_get_text (GTK_EDITABLE (box->field[1]));
  info.author   = gtk_editable_get_text (GTK_EDITABLE (box->field[2]));
  info.keywords = gtk_editable_get_text (GTK_EDITABLE (box->field[3]));
  info.comments = gtk_editable_get_text (GTK_EDITABLE (box->field[4]));

  had = w42_pt_get_info (w42_document_pt (doc));
  changed = g_strcmp0 (had->title, info.title) || g_strcmp0 (had->subject, info.subject) ||
            g_strcmp0 (had->author, info.author) || g_strcmp0 (had->keywords, info.keywords) ||
            g_strcmp0 (had->comments, info.comments);
  if (changed)
    {
      w42_pt_set_info (w42_document_pt (doc), &info);
      w42_document_mark_unsaved (doc);
      w42_document_touch (doc);
    }
  gtk_window_close (GTK_WINDOW (box->window));
}

void
w42_summary_dialog_show (GtkWindow *parent, W42View *view)
{
  static const char *const labels[] = { "_Title:", "_Subject:", "_Author:", "_Keywords:", "_Comments:" };
  SummaryBox *box;
  GtkWidget *content, *grid;
  const W42DocInfo *info;
  const char *values[5];

  g_return_if_fail (W42_IS_VIEW (view));
  if (w42_view_get_document (view) == NULL)
    return;

  info = w42_pt_get_info (w42_document_pt (w42_view_get_document (view)));
  values[0] = info->title;
  values[1] = info->subject;
  values[2] = info->author;
  values[3] = info->keywords;
  values[4] = info->comments;

  box = g_new0 (SummaryBox, 1);
  box->view = view;
  box->window = dialog_shell (parent, "Summary Info", &content, view);
  g_object_set_data_full (G_OBJECT (box->window), "w42-box", box, g_free);

  grid = group (content, "This document");
  for (int i = 0; i < 5; i++)
    {
      GtkWidget *label = gtk_label_new_with_mnemonic (labels[i]);
      char *name;

      box->field[i] = gtk_entry_new ();
      /* An author who has said nothing is the person using the program. */
      name = i == 2 && values[i] == NULL ? w42_settings_get_string ("user-name", "") : NULL;
      if (values[i] != NULL)
        gtk_editable_set_text (GTK_EDITABLE (box->field[i]), values[i]);
      else if (name != NULL && *name != '\0')
        gtk_editable_set_text (GTK_EDITABLE (box->field[i]), name);
      else if (i == 2)
        {
          const char *real = g_get_real_name ();

          gtk_editable_set_text (GTK_EDITABLE (box->field[i]),
                                 real != NULL && !g_str_equal (real, "Unknown") ? real : g_get_user_name ());
        }
      g_free (name);
      gtk_entry_set_activates_default (GTK_ENTRY (box->field[i]), TRUE);
      gtk_widget_set_size_request (box->field[i], 260, -1);
      gtk_label_set_xalign (GTK_LABEL (label), 0.0);
      gtk_label_set_mnemonic_widget (GTK_LABEL (label), box->field[i]);
      gtk_grid_attach (GTK_GRID (grid), label, 0, i, 1, 1);
      gtk_grid_attach (GTK_GRID (grid), box->field[i], 1, i, 1, 1);
    }

  button_row (content, box->window, G_CALLBACK (summary_ok), box);
  gtk_window_present (GTK_WINDOW (box->window));
  gtk_widget_grab_focus (box->field[0]);
}
