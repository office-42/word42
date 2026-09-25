/* w42-macro.c - Tools > Macro: Word42 Basic over a document
 *
 * Copyright (C) 2026 Andreas Røsdal
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * This program is free software: you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by the Free
 * Software Foundation, either version 3 of the License, or (at your option)
 * any later version.  See the LICENSE file for the full text.
 *
 * The object model is Word's, as far as Word42 has the thing behind it:
 * Selection, with its Font, ParagraphFormat and Find; ActiveDocument;
 * Application; Documents; and the global MsgBox, InputBox and the string
 * functions VBA had.  Each member is a native function under the name
 * the translator makes of the dotted one -- Selection.Font.Bold read is
 * SELECTION_FONT_BOLD(), set is SELECTION_FONT_BOLD_SET(v) -- and MY-BASIC
 * calls it with the interpreter, which carries this file's context in its
 * userdata.
 */

#include "w42-macro.h"

#include "w42-vba.h"
#include "w42-window.h"
#include "w42-search.h"
#include "w42-io.h"
#include "my_basic.h"

#include <glib/gi18n.h>
#include <glib/gstdio.h>
#include <string.h>
#include <math.h>
#include <errno.h>

/* A macro that never ends would hang the program, since it runs on the
 * main loop: this many statements is minutes of work, and past it the
 * macro is stopped. */
#define STEP_LIMIT 20000000

typedef struct _Modal Modal;

typedef struct {
  GtkWindow     *parent;
  W42View       *view;
  GString       *output;        /* may be NULL */
  char          *error;         /* the engine's message */
  int            error_row;
  guint64        steps;
  gboolean       runaway;
  /* The window or the view went while the macro ran: closed, or the pane
   * unsplit, from the main loop a MsgBox runs.  Both are held until the
   * macro ends, so nothing is freed under it, but they are off the
   * screen, and a macro carrying on in them works where nobody sees. */
  gboolean       gone;
  Modal         *modal;         /* the box being answered, if any */
  /* Every window and view the macro has worked in, held until it ends;
   * and the documents it has worked on, each of which gets its edits as
   * one undo step once `grouping`. */
  GPtrArray     *held;
  GPtrArray     *docs;
  gboolean       grouping;
  /* Selection.Find's properties, for an Execute with no arguments. */
  char          *find_text;
  char          *replace_text;
  gboolean       match_case, whole_word, forward, wrap;
  /* A run of Execute Replace:=wdReplaceOne: the view it is in, the
   * selection the last one left -- which is how the next knows it
   * carries the run on -- where the run began, and whether its search
   * has wrapped round the end of the document, or come back round
   * past where it began. */
  W42View       *replace_view;
  gsize          replace_start, replace_end;
  gsize          replace_origin;
  gboolean       replace_wrapped, replace_done;
} Ctx;

static Ctx *
ctx_of (struct mb_interpreter_t *s)
{
  void *d = NULL;

  mb_get_userdata (s, &d);
  return d;
}

static W42PieceTable *
pt_of (Ctx *c)
{
  return w42_document_pt (w42_view_get_document (c->view));
}

static void on_gone (GtkWidget *widget, gpointer data);

/* Makes `view`, in `parent`, the one the macro works in: the one it was
 * run in, and after that each document Documents.Add or Documents.Open
 * makes, which becomes the active document as it does in Word.  Both
 * are held until the macro ends, since every native reaches through
 * them and a MsgBox runs the main loop, from which they can be closed.
 * Held, a closed window is not disposed until it is let go, so
 * "destroy" does not come in time; what does come at once, to a window
 * closed and to a pane taken away, is "unrealize". */
static void
ctx_enter (Ctx *c, GtkWindow *parent, W42View *view)
{
  W42Document *doc = w42_view_get_document (view);

  c->parent = parent;
  c->view = view;
  c->gone = FALSE;
  g_ptr_array_add (c->held, g_object_ref (view));
  g_signal_connect (view, "unrealize", G_CALLBACK (on_gone), c);
  if (parent != NULL)
    {
      g_ptr_array_add (c->held, g_object_ref (parent));
      g_signal_connect (parent, "unrealize", G_CALLBACK (on_gone), c);
    }
  if (doc != NULL && !g_ptr_array_find (c->docs, doc, NULL))
    {
      g_ptr_array_add (c->docs, g_object_ref (doc));
      if (c->grouping)
        w42_pt_begin_group (w42_document_pt (doc));
    }
}

/* The view a window edits in: the first in it, which is the only one a
 * window just made has. */
static W42View *
window_view (GtkWidget *widget)
{
  if (W42_IS_VIEW (widget))
    return W42_VIEW (widget);
  for (GtkWidget *child = gtk_widget_get_first_child (widget); child != NULL;
       child = gtk_widget_get_next_sibling (child))
    {
      W42View *view = window_view (child);

      if (view != NULL)
        return view;
    }
  return NULL;
}

/* ---------------------------------------------------------------------- */
/* Arguments and results                                                   */
/* ---------------------------------------------------------------------- */

/* The translator passes nil for an argument left out, and MY-BASIC's own
 * pops refuse a nil, so every argument comes through mb_pop_value. */
static int
arg_value (struct mb_interpreter_t *s, void **l, mb_value_t *v)
{
  mb_make_nil (*v);
  if (!mb_has_arg (s, l))
    return MB_FUNC_OK;
  return mb_pop_value (s, l, v);
}

static int
arg_string (struct mb_interpreter_t *s, void **l, char **out)
{
  mb_value_t v;

  *out = NULL;
  mb_check (arg_value (s, l, &v));
  switch (v.type)
    {
    case MB_DT_STRING: *out = g_strdup (v.value.string); break;
    case MB_DT_INT:    *out = g_strdup_printf ("%d", v.value.integer); break;
    case MB_DT_REAL:   *out = g_strdup_printf ("%g", v.value.float_point); break;
    default:           *out = g_strdup (""); break;
    }
  return MB_FUNC_OK;
}

static int
arg_int (struct mb_interpreter_t *s, void **l, int *out, int fallback)
{
  mb_value_t v;

  *out = fallback;
  mb_check (arg_value (s, l, &v));
  if (v.type == MB_DT_INT)
    *out = v.value.integer;
  else if (v.type == MB_DT_REAL)
    *out = (int) v.value.float_point;
  else if (v.type == MB_DT_STRING)
    *out = (int) g_ascii_strtod (v.value.string, NULL);
  return MB_FUNC_OK;
}

static int
arg_real (struct mb_interpreter_t *s, void **l, double *out, double fallback)
{
  mb_value_t v;

  *out = fallback;
  mb_check (arg_value (s, l, &v));
  if (v.type == MB_DT_INT)
    *out = v.value.integer;
  else if (v.type == MB_DT_REAL)
    *out = v.value.float_point;
  else if (v.type == MB_DT_STRING)
    *out = g_ascii_strtod (v.value.string, NULL);
  return MB_FUNC_OK;
}

/* VBA's True is -1, MY-BASIC's is 1; any non-zero is on. */
static int
arg_bool (struct mb_interpreter_t *s, void **l, gboolean *out, gboolean fallback)
{
  mb_value_t v;

  *out = fallback;
  mb_check (arg_value (s, l, &v));
  if (v.type == MB_DT_INT)
    *out = v.value.integer != 0;
  else if (v.type == MB_DT_REAL)
    *out = v.value.float_point != 0.0;
  return MB_FUNC_OK;
}

static int
push_string (struct mb_interpreter_t *s, void **l, const char *str)
{
  const char *text = str != NULL ? str : "";

  return mb_push_string (s, l, mb_memdup (text, (unsigned) strlen (text) + 1));
}

/* Every native opens and closes its bracket; these keep that in one
 * place. */
#define OPEN()  mb_check (mb_attempt_open_bracket (s, l))
#define CLOSE() mb_check (mb_attempt_close_bracket (s, l))
#define NATIVE(name) static int name (struct mb_interpreter_t *s, void **l)

/* A native that fails says why in the engine's error, against the
 * statement that called it. */
static int
fail (struct mb_interpreter_t *s, void **l, const char *message)
{
  Ctx *c = ctx_of (s);

  if (c != NULL && c->error == NULL)
    c->error = g_strdup (message);
  return mb_raise_error (s, l, SE_RN_FAILED_TO_OPERATE, MB_FUNC_ERR);
}

/* ---------------------------------------------------------------------- */
/* Message and input boxes, run to completion on a loop of their own       */
/* ---------------------------------------------------------------------- */

struct _Modal {
  GMainLoop *loop;
  GtkWidget *window;
  GtkWidget *entry;
  int        result;
  char      *text;
};

static void
on_modal_button (GtkButton *button, gpointer data)
{
  Modal *m = data;

  m->result = GPOINTER_TO_INT (g_object_get_data (G_OBJECT (button), "w42-result"));
  if (m->entry != NULL)
    m->text = g_strdup (gtk_editable_get_text (GTK_EDITABLE (m->entry)));
  g_main_loop_quit (m->loop);
}

static gboolean
on_modal_close (GtkWindow *window, gpointer data)
{
  Modal *m = data;

  (void) window;
  g_main_loop_quit (m->loop);
  return TRUE;                          /* destroyed by the caller */
}

static void
on_modal_activate (GtkEntry *entry, gpointer data)
{
  Modal *m = data;

  (void) entry;
  m->result = 1;
  m->text = g_strdup (gtk_editable_get_text (GTK_EDITABLE (m->entry)));
  g_main_loop_quit (m->loop);
}

/* The window, and the loop it runs on until a button says which.  A
 * macro is a straight line of statements, so the box has to be answered
 * before the next one runs, which no callback can do: the box gets a main
 * loop of its own, as dialogs had before GTK 4. */
static void
modal_run (Ctx *c, Modal *m, const char *title, const char *prompt,
           const char *const *buttons, const int *results, int n_buttons,
           const char *entry_text)
{
  GtkWindow *parent = c->parent;
  GtkWidget *box = gtk_box_new (GTK_ORIENTATION_VERTICAL, 12);
  GtkWidget *label = gtk_label_new (prompt);
  GtkWidget *row = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 8);

  m->window = gtk_window_new ();
  m->loop = g_main_loop_new (NULL, FALSE);
  gtk_window_set_title (GTK_WINDOW (m->window), title);
  gtk_window_set_transient_for (GTK_WINDOW (m->window), parent);
  gtk_window_set_modal (GTK_WINDOW (m->window), TRUE);
  gtk_window_set_resizable (GTK_WINDOW (m->window), FALSE);
  gtk_widget_add_css_class (m->window, "w42");
  gtk_widget_add_css_class (box, "w42-dialog");
  gtk_widget_set_margin_start (box, 14);
  gtk_widget_set_margin_end (box, 14);
  gtk_widget_set_margin_top (box, 14);
  gtk_widget_set_margin_bottom (box, 14);
  gtk_label_set_wrap (GTK_LABEL (label), TRUE);
  gtk_label_set_max_width_chars (GTK_LABEL (label), 60);
  gtk_label_set_xalign (GTK_LABEL (label), 0.0);
  gtk_box_append (GTK_BOX (box), label);

  if (entry_text != NULL)
    {
      m->entry = gtk_entry_new ();
      gtk_editable_set_text (GTK_EDITABLE (m->entry), entry_text);
      gtk_widget_set_size_request (m->entry, 320, -1);
      g_signal_connect (m->entry, "activate", G_CALLBACK (on_modal_activate), m);
      gtk_box_append (GTK_BOX (box), m->entry);
    }

  gtk_widget_set_halign (row, GTK_ALIGN_END);
  for (int i = 0; i < n_buttons; i++)
    {
      GtkWidget *b = gtk_button_new_with_mnemonic (_(buttons[i]));

      gtk_widget_set_size_request (b, 80, -1);
      g_object_set_data (G_OBJECT (b), "w42-result", GINT_TO_POINTER (results[i]));
      g_signal_connect (b, "clicked", G_CALLBACK (on_modal_button), m);
      gtk_box_append (GTK_BOX (row), b);
      if (i == 0)
        gtk_window_set_default_widget (GTK_WINDOW (m->window), b);
    }
  gtk_box_append (GTK_BOX (box), row);
  gtk_window_set_child (GTK_WINDOW (m->window), box);
  g_signal_connect (m->window, "close-request", G_CALLBACK (on_modal_close), m);

  gtk_window_present (GTK_WINDOW (m->window));
  if (m->entry != NULL)
    gtk_widget_grab_focus (m->entry);
  c->modal = m;
  g_main_loop_run (m->loop);
  c->modal = NULL;
  gtk_window_destroy (GTK_WINDOW (m->window));
  g_main_loop_unref (m->loop);
  m->loop = NULL;
}

/* MsgBox(prompt, buttons, title): the button pressed, by VBA's numbers. */
NATIVE (n_msgbox)
{
  Ctx *c = ctx_of (s);
  char *prompt, *title;
  int buttons;
  Modal m = { NULL, NULL, NULL, 2, NULL };
  static const char *const sets[][3] = {
    { N_("_OK"), NULL, NULL }, { N_("_OK"), N_("Cancel"), NULL },
    { N_("_Abort"), N_("_Retry"), N_("_Ignore") },
    { N_("_Yes"), N_("_No"), N_("Cancel") }, { N_("_Yes"), N_("_No"), NULL },
    { N_("_Retry"), N_("Cancel"), NULL }
  };
  static const int codes[][3] = {
    { 1, 0, 0 }, { 1, 2, 0 }, { 3, 4, 5 }, { 6, 7, 2 }, { 6, 7, 0 }, { 4, 2, 0 }
  };
  int set, n;

  OPEN ();
  mb_check (arg_string (s, l, &prompt));
  mb_check (arg_int (s, l, &buttons, 0));
  mb_check (arg_string (s, l, &title));
  CLOSE ();

  set = CLAMP (buttons & 0x0F, 0, 5);
  n = sets[set][2] != NULL ? 3 : (sets[set][1] != NULL ? 2 : 1);
  /* With no Cancel to press, closing the box is OK, as Word had it. */
  m.result = set == 0 ? 1 : (set == 4 ? 7 : 2);
  if (!c->gone)
    modal_run (c, &m, *title != '\0' ? title : "Word42", prompt,
               sets[set], codes[set], n, NULL);
  g_free (prompt);
  g_free (title);
  if (c->gone)
    return fail (s, l, _("The document was closed while the macro ran"));
  return mb_push_int (s, l, m.result);
}

/* InputBox(prompt, title, default): what was typed, or "" for Cancel. */
NATIVE (n_inputbox)
{
  Ctx *c = ctx_of (s);
  char *prompt, *title, *initial;
  static const char *const buttons[] = { N_("_OK"), N_("Cancel") };
  static const int codes[] = { 1, 2 };
  Modal m = { NULL, NULL, NULL, 2, NULL };
  int rc;

  OPEN ();
  mb_check (arg_string (s, l, &prompt));
  mb_check (arg_string (s, l, &title));
  mb_check (arg_string (s, l, &initial));
  CLOSE ();

  if (!c->gone)
    modal_run (c, &m, *title != '\0' ? title : "Word42", prompt,
               buttons, codes, 2, initial);
  if (c->gone)
    rc = fail (s, l, _("The document was closed while the macro ran"));
  else
    rc = push_string (s, l, m.result == 1 && m.text != NULL ? m.text : "");
  g_free (m.text);
  g_free (prompt);
  g_free (title);
  g_free (initial);
  return rc;
}

/* Debug.Print a, b: to the editor's output pane, and the status bar. */
NATIVE (n_debug_print)
{
  Ctx *c = ctx_of (s);
  GString *line = g_string_new (NULL);

  OPEN ();
  while (mb_has_arg (s, l))
    {
      char *t;

      mb_check (arg_string (s, l, &t));
      if (line->len > 0)
        g_string_append_c (line, ' ');
      g_string_append (line, t);
      g_free (t);
    }
  CLOSE ();
  if (c->output != NULL)
    {
      g_string_append (c->output, line->str);
      g_string_append_c (c->output, '\n');
    }
  if (W42_IS_WINDOW (c->parent))
    w42_window_flash_status (W42_WINDOW (c->parent), line->str);
  g_string_free (line, TRUE);
  return MB_FUNC_OK;
}

/* ---------------------------------------------------------------------- */
/* Strings and numbers: what VBA had and MY-BASIC has not, or has 0-based */
/* ---------------------------------------------------------------------- */

NATIVE (n_cat)
{
  char *a, *b, *r;
  int rc;

  OPEN ();
  mb_check (arg_string (s, l, &a));
  mb_check (arg_string (s, l, &b));
  CLOSE ();
  r = g_strconcat (a, b, NULL);
  rc = push_string (s, l, r);
  g_free (a); g_free (b); g_free (r);
  return rc;
}

NATIVE (n_idiv)
{
  int a, b;

  OPEN ();
  mb_check (arg_int (s, l, &a, 0));
  mb_check (arg_int (s, l, &b, 1));
  CLOSE ();
  if (b == 0)
    return fail (s, l, _("Division by zero"));
  /* The one quotient an int cannot hold, and the processor traps on it. */
  if (a == G_MININT && b == -1)
    /* Translators: a calculation in a macro gave a number too big to hold. */
    return fail (s, l, _("Overflow"));
  return mb_push_int (s, l, a / b);
}

NATIVE (n_xor)
{
  int a, b;

  OPEN ();
  mb_check (arg_int (s, l, &a, 0));
  mb_check (arg_int (s, l, &b, 0));
  CLOSE ();
  return mb_push_int (s, l, (a != 0) != (b != 0));
}

/* Mid(s, start[, length]), 1-based as VBA counted. */
NATIVE (n_mid)
{
  char *str;
  int start, length;
  glong n;
  char *r;
  int rc;

  OPEN ();
  mb_check (arg_string (s, l, &str));
  mb_check (arg_int (s, l, &start, 1));
  mb_check (arg_int (s, l, &length, G_MAXINT));
  CLOSE ();
  n = g_utf8_strlen (str, -1);
  start = MAX (start, 1);
  if (start > n || length <= 0)
    r = g_strdup ("");
  else
    {
      const char *from = g_utf8_offset_to_pointer (str, start - 1);
      glong take = MIN ((glong) length, n - (start - 1));
      const char *to = g_utf8_offset_to_pointer (from, take);

      r = g_strndup (from, (gsize) (to - from));
    }
  rc = push_string (s, l, r);
  g_free (str); g_free (r);
  return rc;
}

NATIVE (n_left)
{
  char *str, *r;
  int length;
  glong n;
  int rc;

  OPEN ();
  mb_check (arg_string (s, l, &str));
  mb_check (arg_int (s, l, &length, 0));
  CLOSE ();
  n = g_utf8_strlen (str, -1);
  length = CLAMP (length, 0, (int) n);
  r = g_strndup (str, (gsize) (g_utf8_offset_to_pointer (str, length) - str));
  rc = push_string (s, l, r);
  g_free (str); g_free (r);
  return rc;
}

NATIVE (n_right)
{
  char *str;
  int length;
  glong n;
  int rc;

  OPEN ();
  mb_check (arg_string (s, l, &str));
  mb_check (arg_int (s, l, &length, 0));
  CLOSE ();
  n = g_utf8_strlen (str, -1);
  length = CLAMP (length, 0, (int) n);
  rc = push_string (s, l, g_utf8_offset_to_pointer (str, n - length));
  g_free (str);
  return rc;
}

NATIVE (n_len)
{
  char *str;
  glong n;

  OPEN ();
  mb_check (arg_string (s, l, &str));
  CLOSE ();
  n = g_utf8_strlen (str, -1);
  g_free (str);
  return mb_push_int (s, l, (int) n);
}

static int
string_map (struct mb_interpreter_t *s, void **l, char *(*fn) (const char *, gssize))
{
  char *str, *r;
  int rc;

  OPEN ();
  mb_check (arg_string (s, l, &str));
  CLOSE ();
  r = fn (str, -1);
  rc = push_string (s, l, r);
  g_free (str); g_free (r);
  return rc;
}

static char *trim_both (const char *str, gssize len) { (void) len; return g_strstrip (g_strdup (str)); }
static char *trim_left (const char *str, gssize len) { (void) len; return g_strchug (g_strdup (str)); }
static char *trim_right (const char *str, gssize len) { (void) len; return g_strchomp (g_strdup (str)); }
static char *reverse (const char *str, gssize len) { (void) len; return g_utf8_strreverse (str, -1); }
static char *same (const char *str, gssize len) { (void) len; return g_strdup (str); }

NATIVE (n_ucase) { return string_map (s, l, g_utf8_strup); }
NATIVE (n_lcase) { return string_map (s, l, g_utf8_strdown); }
NATIVE (n_trim)  { return string_map (s, l, trim_both); }
NATIVE (n_ltrim) { return string_map (s, l, trim_left); }
NATIVE (n_rtrim) { return string_map (s, l, trim_right); }
NATIVE (n_strreverse) { return string_map (s, l, reverse); }
NATIVE (n_cstr)  { return string_map (s, l, same); }

/* InStr([start,] string1, string2): the 1-based place of one string in
 * the other, or 0. */
NATIVE (n_instr)
{
  mb_value_t first;
  int start = 1;
  char *hay = NULL, *needle = NULL;
  int result = 0;

  OPEN ();
  mb_check (arg_value (s, l, &first));
  if (first.type == MB_DT_INT || first.type == MB_DT_REAL)
    {
      start = first.type == MB_DT_INT ? first.value.integer : (int) first.value.float_point;
      mb_check (arg_string (s, l, &hay));
    }
  else
    hay = g_strdup (first.type == MB_DT_STRING ? first.value.string : "");
  mb_check (arg_string (s, l, &needle));
  CLOSE ();
  {
    glong n = g_utf8_strlen (hay, -1);

    if (start >= 1 && start - 1 <= n)
      {
        const char *from = g_utf8_offset_to_pointer (hay, start - 1);
        const char *at = *needle != '\0' ? strstr (from, needle) : from;

        if (at != NULL)
          result = (int) g_utf8_pointer_to_offset (hay, at) + 1;
      }
  }
  g_free (hay); g_free (needle);
  return mb_push_int (s, l, result);
}

/* Replace(expression, find, replace). */
NATIVE (n_replace)
{
  char *expr, *find, *with;
  int rc;

  OPEN ();
  mb_check (arg_string (s, l, &expr));
  mb_check (arg_string (s, l, &find));
  mb_check (arg_string (s, l, &with));
  CLOSE ();
  if (*find == '\0')
    rc = push_string (s, l, expr);
  else
    {
      char **parts = g_strsplit (expr, find, -1);
      char *r = g_strjoinv (with, parts);

      rc = push_string (s, l, r);
      g_strfreev (parts);
      g_free (r);
    }
  g_free (expr); g_free (find); g_free (with);
  return rc;
}

NATIVE (n_space)
{
  int n;
  char *r;
  int rc;

  OPEN ();
  mb_check (arg_int (s, l, &n, 0));
  CLOSE ();
  r = g_strnfill ((gsize) MAX (n, 0), ' ');
  rc = push_string (s, l, r);
  g_free (r);
  return rc;
}

/* String(n, character). */
NATIVE (n_string)
{
  int n;
  char *ch;
  GString *r = g_string_new (NULL);
  int rc;

  OPEN ();
  mb_check (arg_int (s, l, &n, 0));
  mb_check (arg_string (s, l, &ch));
  CLOSE ();
  for (int i = 0; i < n && *ch != '\0'; i++)
    g_string_append_unichar (r, g_utf8_get_char (ch));
  rc = push_string (s, l, r->str);
  g_string_free (r, TRUE);
  g_free (ch);
  return rc;
}

NATIVE (n_cint)
{
  double v;

  OPEN ();
  mb_check (arg_real (s, l, &v, 0.0));
  CLOSE ();
  return mb_push_int (s, l, (int) lround (v));
}

NATIVE (n_int)
{
  double v;

  OPEN ();
  mb_check (arg_real (s, l, &v, 0.0));
  CLOSE ();
  return mb_push_int (s, l, (int) floor (v));
}

NATIVE (n_cdbl)
{
  double v;

  OPEN ();
  mb_check (arg_real (s, l, &v, 0.0));
  CLOSE ();
  return mb_push_real (s, l, v);
}

NATIVE (n_isnumeric)
{
  char *str;
  char *end = NULL;
  gboolean yes;

  OPEN ();
  mb_check (arg_string (s, l, &str));
  CLOSE ();
  g_strstrip (str);
  g_ascii_strtod (str, &end);
  yes = *str != '\0' && end != NULL && *end == '\0';
  g_free (str);
  return mb_push_int (s, l, yes);
}

/* IIf(condition, a, b).  A string MY-BASIC hands over lives only until
 * the next argument is popped -- "odd" & i is freed while b is worked
 * out -- so each is copied as it comes, and the one chosen goes back as
 * a string of its own. */
NATIVE (n_iif)
{
  gboolean cond;
  mb_value_t a, b;
  char *sa = NULL, *sb = NULL;
  int rc;

  OPEN ();
  mb_check (arg_bool (s, l, &cond, FALSE));
  mb_check (arg_value (s, l, &a));
  if (a.type == MB_DT_STRING)
    sa = g_strdup (a.value.string);
  rc = arg_value (s, l, &b);
  if (rc == MB_FUNC_OK && b.type == MB_DT_STRING)
    sb = g_strdup (b.value.string);
  if (rc == MB_FUNC_OK)
    rc = mb_attempt_close_bracket (s, l);
  if (rc == MB_FUNC_OK)
    {
      const char *str = cond ? sa : sb;

      rc = str != NULL ? push_string (s, l, str) : mb_push_value (s, l, cond ? a : b);
    }
  g_free (sa);
  g_free (sb);
  return rc;
}

NATIVE (n_hex)
{
  int v;
  char *r;
  int rc;

  OPEN ();
  mb_check (arg_int (s, l, &v, 0));
  CLOSE ();
  r = g_strdup_printf ("%X", (unsigned) v);
  rc = push_string (s, l, r);
  g_free (r);
  return rc;
}

static int
push_date (struct mb_interpreter_t *s, void **l, const char *format)
{
  GDateTime *now = g_date_time_new_now_local ();
  char *r = g_date_time_format (now, format);
  int rc = push_string (s, l, r);

  g_free (r);
  g_date_time_unref (now);
  return rc;
}

NATIVE (n_now)  { OPEN (); CLOSE (); return push_date (s, l, "%Y-%m-%d %H:%M:%S"); }
NATIVE (n_date) { OPEN (); CLOSE (); return push_date (s, l, "%Y-%m-%d"); }
NATIVE (n_time) { OPEN (); CLOSE (); return push_date (s, l, "%H:%M:%S"); }
NATIVE (n_year)  { OPEN (); if (mb_has_arg (s, l)) { mb_value_t v; mb_check (arg_value (s, l, &v)); } CLOSE (); { GDateTime *t = g_date_time_new_now_local (); int y = g_date_time_get_year (t); g_date_time_unref (t); return mb_push_int (s, l, y); } }
NATIVE (n_month) { OPEN (); if (mb_has_arg (s, l)) { mb_value_t v; mb_check (arg_value (s, l, &v)); } CLOSE (); { GDateTime *t = g_date_time_new_now_local (); int y = g_date_time_get_month (t); g_date_time_unref (t); return mb_push_int (s, l, y); } }
NATIVE (n_day)   { OPEN (); if (mb_has_arg (s, l)) { mb_value_t v; mb_check (arg_value (s, l, &v)); } CLOSE (); { GDateTime *t = g_date_time_new_now_local (); int y = g_date_time_get_day_of_month (t); g_date_time_unref (t); return mb_push_int (s, l, y); } }

NATIVE (n_timer)
{
  OPEN (); CLOSE ();
  return mb_push_real (s, l, (double) (g_get_real_time () % (G_GINT64_CONSTANT (86400) * G_USEC_PER_SEC)) / G_USEC_PER_SEC);
}

/* Format(value[, format]): the number or text as it is; a format string
 * of zeros and a point fixes the decimals. */
NATIVE (n_format)
{
  mb_value_t v;
  char *str = NULL, *fmt = NULL, *r;
  int rc;

  OPEN ();
  mb_check (arg_value (s, l, &v));
  /* Copied before the format is popped, which frees it: see IIf. */
  if (v.type == MB_DT_STRING)
    str = g_strdup (v.value.string);
  rc = arg_string (s, l, &fmt);
  if (rc == MB_FUNC_OK)
    rc = mb_attempt_close_bracket (s, l);
  if (rc != MB_FUNC_OK)
    {
      g_free (str);
      g_free (fmt);
      return rc;
    }
  if (str != NULL)
    r = g_steal_pointer (&str);
  else
    {
      double d = v.type == MB_DT_INT ? v.value.integer : v.type == MB_DT_REAL ? v.value.float_point : 0.0;
      const char *dot = strchr (fmt, '.');

      if (dot != NULL)
        r = g_strdup_printf ("%.*f", (int) strlen (dot + 1), d);
      else if (v.type == MB_DT_INT)
        r = g_strdup_printf ("%d", v.value.integer);
      else
        r = g_strdup_printf ("%g", d);
    }
  rc = push_string (s, l, r);
  g_free (fmt); g_free (r);
  return rc;
}

/* ---------------------------------------------------------------------- */
/* Selection                                                               */
/* ---------------------------------------------------------------------- */

static void
sel_get (Ctx *c, gsize *anchor, gsize *caret)
{
  gsize start, end;

  w42_view_get_selection_bounds (c->view, &start, &end);
  *caret = w42_view_get_caret (c->view);
  *anchor = (*caret == end) ? start : end;
}

static void
sel_set (Ctx *c, gsize anchor, gsize caret)
{
  w42_view_select_range (c->view, anchor, caret);
}

/* A run of text into the document: vbCr and vbLf end paragraphs, as
 * TypeText's did in Word. */
static void
type_text (Ctx *c, const char *text)
{
  const char *p = text;

  while (*p != '\0')
    {
      const char *q = p;

      while (*q != '\0' && *q != '\r' && *q != '\n')
        q++;
      if (q > p)
        {
          char *piece = g_strndup (p, (gsize) (q - p));

          w42_view_insert_text (c->view, piece);
          g_free (piece);
        }
      if (*q == '\0')
        break;
      w42_view_insert_paragraph (c->view);
      if (*q == '\r' && q[1] == '\n')
        q++;
      p = q + 1;
    }
}

NATIVE (n_sel_typetext)
{
  Ctx *c = ctx_of (s);
  char *text;

  OPEN ();
  mb_check (arg_string (s, l, &text));
  CLOSE ();
  type_text (c, text);
  g_free (text);
  return MB_FUNC_OK;
}

NATIVE (n_sel_typeparagraph)
{
  Ctx *c = ctx_of (s);

  OPEN (); CLOSE ();
  w42_view_insert_paragraph (c->view);
  return MB_FUNC_OK;
}

NATIVE (n_sel_typebackspace)
{
  Ctx *c = ctx_of (s);
  W42PieceTable *pt = pt_of (c);

  OPEN (); CLOSE ();
  if (!w42_view_has_selection (c->view))
    {
      gsize caret = w42_view_get_caret (c->view);
      gsize prev = w42_pt_prev_pos (pt, caret);

      if (prev == caret)
        return MB_FUNC_OK;
      sel_set (c, prev, caret);
    }
  w42_view_clear (c->view);
  return MB_FUNC_OK;
}

/* Moving by a unit: the position `count` units on from `pos`, in `dir`. */
static gsize
move_by (Ctx *c, gsize pos, int unit, int count, int dir)
{
  W42PieceTable *pt = pt_of (c);
  W42Layout *layout = w42_view_get_layout (c->view);

  for (int i = 0; i < count; i++)
    {
      gsize next = pos;

      switch (unit)
        {
        case 2:                         /* wdWord */
          {
            /* Over the word, then the space after it; the other way, over
             * the space, then the word before it. */
            gsize length = w42_pt_length (pt);

            if (dir > 0)
              {
                gboolean in_space = FALSE;

                while (next < length)
                  {
                    char *ch = w42_pt_get_text (pt, next, 1);
                    gboolean space = ch == NULL || *ch == '\0' || g_ascii_isspace (*ch);
                    gsize after = w42_pt_next_pos (pt, next);

                    g_free (ch);
                    if (after == next)
                      break;
                    if (in_space && !space)
                      break;
                    if (space)
                      in_space = TRUE;
                    next = after;
                  }
              }
            else
              {
                gboolean in_word = FALSE;

                while (next > 0)
                  {
                    gsize before = w42_pt_prev_pos (pt, next);
                    char *ch;
                    gboolean space;

                    if (before == next)
                      break;
                    ch = w42_pt_get_text (pt, before, 1);
                    space = ch == NULL || *ch == '\0' || g_ascii_isspace (*ch);
                    g_free (ch);
                    if (in_word && space)
                      break;
                    if (!space)
                      in_word = TRUE;
                    next = before;
                  }
              }
          }
          break;
        case 4:                         /* wdParagraph */
          {
            /* A position just before a paragraph mark is the end of the
             * paragraph before it, and the only place the caret has in
             * an empty one: counted as the next paragraph's, an empty
             * paragraph was stepped over, and MoveUp there went down. */
            gsize at = pos > 0 && w42_pt_is_block_mark (pt, pos) ? pos - 1 : pos;

            if (dir > 0)
              next = w42_pt_clamp_pos (pt, w42_pt_paragraph_end (pt, at) + 1);
            else
              {
                gsize mark = w42_pt_paragraph_start (pt, at);

                if (mark + 1 == pos && mark > 0)
                  mark = w42_pt_paragraph_start (pt, mark - 1);
                next = w42_pt_clamp_pos (pt, mark + 1);
              }
          }
          break;
        case 5:                         /* wdLine */
          {
            double want = -1.0;

            next = w42_layout_move_line (layout, pos, dir, &want);
          }
          break;
        case 6:                         /* wdStory */
          next = dir > 0 ? w42_pt_length (pt) : w42_pt_first_caret_pos (pt);
          break;
        default:                        /* wdCharacter */
          next = dir > 0 ? w42_pt_next_pos (pt, pos) : w42_pt_prev_pos (pt, pos);
          break;
        }
      if (next == pos)
        break;
      pos = next;
    }
  return pos;
}

static int
move_native (struct mb_interpreter_t *s, void **l, int dir, int default_unit)
{
  Ctx *c = ctx_of (s);
  int unit, count, extend;
  gsize anchor, caret, to;

  OPEN ();
  mb_check (arg_int (s, l, &unit, default_unit));
  mb_check (arg_int (s, l, &count, 1));
  mb_check (arg_int (s, l, &extend, 0));
  CLOSE ();
  sel_get (c, &anchor, &caret);
  to = move_by (c, caret, unit, MAX (count, 0), dir);
  sel_set (c, extend == 1 ? anchor : to, to);
  return mb_push_int (s, l, (int) count);
}

NATIVE (n_sel_moveright) { return move_native (s, l, +1, 1); }
NATIVE (n_sel_moveleft)  { return move_native (s, l, -1, 1); }
NATIVE (n_sel_movedown)  { return move_native (s, l, +1, 5); }
NATIVE (n_sel_moveup)    { return move_native (s, l, -1, 5); }

static int
key_native (struct mb_interpreter_t *s, void **l, gboolean home)
{
  Ctx *c = ctx_of (s);
  W42Layout *layout = w42_view_get_layout (c->view);
  W42PieceTable *pt = pt_of (c);
  int unit, extend;
  gsize anchor, caret, to;

  OPEN ();
  mb_check (arg_int (s, l, &unit, 5));
  mb_check (arg_int (s, l, &extend, 0));
  CLOSE ();
  sel_get (c, &anchor, &caret);
  if (unit == 6)
    to = home ? w42_pt_first_caret_pos (pt) : w42_pt_length (pt);
  else
    to = home ? w42_layout_line_start (layout, caret) : w42_layout_line_end (layout, caret);
  sel_set (c, extend == 1 ? anchor : to, to);
  return mb_push_int (s, l, 1);
}

NATIVE (n_sel_homekey) { return key_native (s, l, TRUE); }
NATIVE (n_sel_endkey)  { return key_native (s, l, FALSE); }

NATIVE (n_sel_collapse)
{
  Ctx *c = ctx_of (s);
  int direction;
  gsize start, end;

  OPEN ();
  mb_check (arg_int (s, l, &direction, 1));
  CLOSE ();
  w42_view_get_selection_bounds (c->view, &start, &end);
  sel_set (c, direction == 1 ? start : end, direction == 1 ? start : end);
  return MB_FUNC_OK;
}

NATIVE (n_sel_wholestory)
{
  Ctx *c = ctx_of (s);

  OPEN (); CLOSE ();
  w42_view_select_all (c->view);
  return MB_FUNC_OK;
}

/* Delete([unit[, count]]): the selection, or what the caret is before. */
NATIVE (n_sel_delete)
{
  Ctx *c = ctx_of (s);
  int unit, count;

  OPEN ();
  mb_check (arg_int (s, l, &unit, 1));
  mb_check (arg_int (s, l, &count, 1));
  CLOSE ();
  if (!w42_view_has_selection (c->view))
    {
      gsize caret = w42_view_get_caret (c->view);
      gsize to = move_by (c, caret, unit, MAX (count, 0), +1);

      if (to == caret)
        return mb_push_int (s, l, 0);
      sel_set (c, caret, to);
    }
  w42_view_clear (c->view);
  return mb_push_int (s, l, 1);
}

NATIVE (n_sel_text)
{
  Ctx *c = ctx_of (s);
  char *text;
  int rc;

  OPEN (); CLOSE ();
  if (w42_view_has_selection (c->view))
    text = w42_view_get_selected_text (c->view);
  else
    {
      /* Word gives the character after the insertion point. */
      W42PieceTable *pt = pt_of (c);
      gsize caret = w42_view_get_caret (c->view);

      text = caret < w42_pt_length (pt) ? w42_pt_get_text (pt, caret, 1) : g_strdup ("");
    }
  rc = push_string (s, l, text);
  g_free (text);
  return rc;
}

NATIVE (n_sel_text_set)
{
  Ctx *c = ctx_of (s);
  char *text;

  OPEN ();
  mb_check (arg_string (s, l, &text));
  CLOSE ();
  if (*text == '\0')
    {
      if (w42_view_has_selection (c->view))
        w42_view_clear (c->view);
    }
  else
    type_text (c, text);
  g_free (text);
  return MB_FUNC_OK;
}

NATIVE (n_sel_start)
{
  Ctx *c = ctx_of (s);
  gsize start, end;

  OPEN (); CLOSE ();
  w42_view_get_selection_bounds (c->view, &start, &end);
  return mb_push_int (s, l, (int) start);
}

NATIVE (n_sel_end)
{
  Ctx *c = ctx_of (s);
  gsize start, end;

  OPEN (); CLOSE ();
  w42_view_get_selection_bounds (c->view, &start, &end);
  return mb_push_int (s, l, (int) end);
}

NATIVE (n_sel_start_set)
{
  Ctx *c = ctx_of (s);
  gsize start, end;
  int v;

  OPEN ();
  mb_check (arg_int (s, l, &v, 0));
  CLOSE ();
  w42_view_get_selection_bounds (c->view, &start, &end);
  start = w42_pt_clamp_pos (pt_of (c), (gsize) MAX (v, 0));
  sel_set (c, start, MAX (start, end));
  return MB_FUNC_OK;
}

NATIVE (n_sel_end_set)
{
  Ctx *c = ctx_of (s);
  gsize start, end;
  int v;

  OPEN ();
  mb_check (arg_int (s, l, &v, 0));
  CLOSE ();
  w42_view_get_selection_bounds (c->view, &start, &end);
  end = w42_pt_clamp_pos (pt_of (c), (gsize) MAX (v, 0));
  sel_set (c, MIN (start, end), end);
  return MB_FUNC_OK;
}

NATIVE (n_sel_copy)  { Ctx *c = ctx_of (s); OPEN (); CLOSE (); w42_view_copy (c->view); return MB_FUNC_OK; }
NATIVE (n_sel_cut)   { Ctx *c = ctx_of (s); OPEN (); CLOSE (); w42_view_cut (c->view); return MB_FUNC_OK; }
NATIVE (n_sel_paste) { Ctx *c = ctx_of (s); OPEN (); CLOSE (); w42_view_paste (c->view); return MB_FUNC_OK; }

NATIVE (n_sel_insertafter)
{
  Ctx *c = ctx_of (s);
  char *text;
  gsize start, end;

  OPEN ();
  mb_check (arg_string (s, l, &text));
  CLOSE ();
  w42_view_get_selection_bounds (c->view, &start, &end);
  sel_set (c, end, end);
  type_text (c, text);
  g_free (text);
  return MB_FUNC_OK;
}

NATIVE (n_sel_insertbefore)
{
  Ctx *c = ctx_of (s);
  char *text;
  gsize start, end;

  OPEN ();
  mb_check (arg_string (s, l, &text));
  CLOSE ();
  w42_view_get_selection_bounds (c->view, &start, &end);
  sel_set (c, start, start);
  type_text (c, text);
  g_free (text);
  return MB_FUNC_OK;
}

/* InsertBreak([Type]): a page break, which is what it puts in when not
 * told, or a section break that starts a new page.  Either takes the
 * selection's place. */
NATIVE (n_sel_insertbreak)
{
  Ctx *c = ctx_of (s);
  int type;

  OPEN ();
  mb_check (arg_int (s, l, &type, 7));
  CLOSE ();
  switch (type)
    {
    case 7:                             /* wdPageBreak */
      w42_view_insert_page_break (c->view);
      break;
    case 2:                             /* wdSectionBreakNextPage */
      w42_view_insert_section_break (c->view);
      break;
    default:
      /* Translators: wdPageBreak and wdSectionBreakNextPage are names in
       * the macro language: keep them in English. */
      return fail (s, l, _("Word42 inserts a wdPageBreak or a wdSectionBreakNextPage, "
                           "and no other kind of break"));
    }
  return MB_FUNC_OK;
}

/* Selection.Words.Count and its kin: the selection's statistics. */
static int
sel_stat (struct mb_interpreter_t *s, void **l, int which)
{
  Ctx *c = ctx_of (s);
  gsize start, end;
  W42Stats st;

  OPEN (); CLOSE ();
  w42_view_get_selection_bounds (c->view, &start, &end);
  w42_pt_statistics_range (pt_of (c), start, end, &st);
  return mb_push_int (s, l, (int) (which == 0 ? st.words : which == 1 ? st.characters : st.paragraphs));
}

NATIVE (n_sel_words_count) { return sel_stat (s, l, 0); }
NATIVE (n_sel_characters_count) { return sel_stat (s, l, 1); }
NATIVE (n_sel_paragraphs_count) { return sel_stat (s, l, 2); }

/* ---- Font ------------------------------------------------------------- */

/* wdToggle turns a switch over; True and False set it. */
static gboolean
switch_value (int value, gboolean now)
{
  if (value == 9999998)
    return !now;
  return value != 0;
}

static int
font_get_switch (struct mb_interpreter_t *s, void **l, int which)
{
  Ctx *c = ctx_of (s);
  W42CharFmt ch;
  int v = 0;

  OPEN (); CLOSE ();
  w42_view_get_char_fmt (c->view, &ch);
  switch (which)
    {
    case 0: v = ch.bold; break;
    case 1: v = ch.italic; break;
    case 2: v = ch.strikeout; break;
    case 3: v = ch.script > 0; break;
    case 4: v = ch.script < 0; break;
    case 5: v = ch.allcaps; break;
    case 6: v = ch.smallcaps; break;
    case 7: v = ch.dstrike; break;
    case 8: v = ch.shadow; break;
    case 9: v = ch.outline; break;
    case 10: v = ch.emboss; break;
    case 11: v = ch.engrave; break;
    default: break;
    }
  return mb_push_int (s, l, v);
}

static int
font_set_switch (struct mb_interpreter_t *s, void **l, int which)
{
  Ctx *c = ctx_of (s);
  W42CharFmt ch;
  int value;
  gboolean on;

  OPEN ();
  mb_check (arg_int (s, l, &value, 1));
  CLOSE ();
  w42_view_get_char_fmt (c->view, &ch);
  switch (which)
    {
    case 0: on = switch_value (value, ch.bold); ch.bold = on;
            w42_view_apply_char_fmt (c->view, W42_CHAR_BOLD, &ch); break;
    case 1: on = switch_value (value, ch.italic); ch.italic = on;
            w42_view_apply_char_fmt (c->view, W42_CHAR_ITALIC, &ch); break;
    case 2: on = switch_value (value, ch.strikeout); ch.strikeout = on;
            w42_view_apply_char_fmt (c->view, W42_CHAR_STRIKEOUT, &ch); break;
    case 3: on = switch_value (value, ch.script > 0); ch.script = on ? 1 : 0;
            w42_view_apply_char_fmt (c->view, W42_CHAR_SCRIPT, &ch); break;
    case 4: on = switch_value (value, ch.script < 0); ch.script = on ? -1 : 0;
            w42_view_apply_char_fmt (c->view, W42_CHAR_SCRIPT, &ch); break;
    case 5: on = switch_value (value, ch.allcaps); ch.allcaps = on;
            w42_view_apply_char_fmt (c->view, W42_CHAR_ALLCAPS, &ch); break;
    case 6: on = switch_value (value, ch.smallcaps); ch.smallcaps = on;
            w42_view_apply_char_fmt (c->view, W42_CHAR_SMALLCAPS, &ch); break;
    case 7: on = switch_value (value, ch.dstrike); ch.dstrike = on;
            w42_view_apply_char_fmt (c->view, W42_CHAR_DSTRIKE, &ch); break;
    case 8: on = switch_value (value, ch.shadow); ch.shadow = on;
            w42_view_apply_char_fmt (c->view, W42_CHAR_SHADOW, &ch); break;
    case 9: on = switch_value (value, ch.outline); ch.outline = on;
            w42_view_apply_char_fmt (c->view, W42_CHAR_OUTLINE, &ch); break;
    case 10: on = switch_value (value, ch.emboss); ch.emboss = on;
            if (on) ch.engrave = 0;
            w42_view_apply_char_fmt (c->view, W42_CHAR_EMBOSS | W42_CHAR_ENGRAVE, &ch); break;
    case 11: on = switch_value (value, ch.engrave); ch.engrave = on;
            if (on) ch.emboss = 0;
            w42_view_apply_char_fmt (c->view, W42_CHAR_EMBOSS | W42_CHAR_ENGRAVE, &ch); break;
    default: break;
    }
  return MB_FUNC_OK;
}

NATIVE (n_font_bold)          { return font_get_switch (s, l, 0); }
NATIVE (n_font_bold_set)      { return font_set_switch (s, l, 0); }
NATIVE (n_font_italic)        { return font_get_switch (s, l, 1); }
NATIVE (n_font_italic_set)    { return font_set_switch (s, l, 1); }
NATIVE (n_font_strike)        { return font_get_switch (s, l, 2); }
NATIVE (n_font_strike_set)    { return font_set_switch (s, l, 2); }
NATIVE (n_font_super)         { return font_get_switch (s, l, 3); }
NATIVE (n_font_super_set)     { return font_set_switch (s, l, 3); }
NATIVE (n_font_sub)           { return font_get_switch (s, l, 4); }
NATIVE (n_font_sub_set)       { return font_set_switch (s, l, 4); }
NATIVE (n_font_allcaps)       { return font_get_switch (s, l, 5); }
NATIVE (n_font_allcaps_set)   { return font_set_switch (s, l, 5); }
NATIVE (n_font_smallcaps)     { return font_get_switch (s, l, 6); }
NATIVE (n_font_smallcaps_set) { return font_set_switch (s, l, 6); }
NATIVE (n_font_dstrike)       { return font_get_switch (s, l, 7); }
NATIVE (n_font_dstrike_set)   { return font_set_switch (s, l, 7); }
NATIVE (n_font_shadow)        { return font_get_switch (s, l, 8); }
NATIVE (n_font_shadow_set)    { return font_set_switch (s, l, 8); }
NATIVE (n_font_outline)       { return font_get_switch (s, l, 9); }
NATIVE (n_font_outline_set)   { return font_set_switch (s, l, 9); }
NATIVE (n_font_emboss)        { return font_get_switch (s, l, 10); }
NATIVE (n_font_emboss_set)    { return font_set_switch (s, l, 10); }
NATIVE (n_font_engrave)       { return font_get_switch (s, l, 11); }
NATIVE (n_font_engrave_set)   { return font_set_switch (s, l, 11); }

/* Underline by Word's numbers: 0 none, 1 single, 2 words, 3 double. */
NATIVE (n_font_underline)
{
  Ctx *c = ctx_of (s);
  W42CharFmt ch;
  int v;

  OPEN (); CLOSE ();
  w42_view_get_char_fmt (c->view, &ch);
  v = ch.underline == W42_UNDERLINE_NONE ? 0 : ch.underline == W42_UNDERLINE_WORDS ? 2
    : ch.underline == W42_UNDERLINE_DOUBLE ? 3 : 1;
  return mb_push_int (s, l, v);
}

NATIVE (n_font_underline_set)
{
  Ctx *c = ctx_of (s);
  W42CharFmt ch;
  int value;

  OPEN ();
  mb_check (arg_int (s, l, &value, 1));
  CLOSE ();
  w42_view_get_char_fmt (c->view, &ch);
  if (value == 9999998)
    ch.underline = ch.underline == W42_UNDERLINE_NONE ? W42_UNDERLINE_SINGLE : W42_UNDERLINE_NONE;
  else
    ch.underline = value == 0 ? W42_UNDERLINE_NONE : value == 2 ? W42_UNDERLINE_WORDS
                 : value == 3 ? W42_UNDERLINE_DOUBLE : W42_UNDERLINE_SINGLE;
  w42_view_apply_char_fmt (c->view, W42_CHAR_UNDERLINE, &ch);
  return MB_FUNC_OK;
}

NATIVE (n_font_size)
{
  Ctx *c = ctx_of (s);
  W42CharFmt ch;

  OPEN (); CLOSE ();
  w42_view_get_char_fmt (c->view, &ch);
  return mb_push_real (s, l, (ch.size > 0 ? ch.size : 20) / 2.0);
}

NATIVE (n_font_size_set)
{
  Ctx *c = ctx_of (s);
  double pt;

  OPEN ();
  mb_check (arg_real (s, l, &pt, 10.0));
  CLOSE ();
  w42_view_set_font_size (c->view, CLAMP ((int) lround (pt * 2), 2, 1638));
  return MB_FUNC_OK;
}

NATIVE (n_font_name)
{
  Ctx *c = ctx_of (s);
  W42CharFmt ch;

  OPEN (); CLOSE ();
  w42_view_get_char_fmt (c->view, &ch);
  return push_string (s, l, ch.family != NULL ? ch.family : "Times New Roman");
}

NATIVE (n_font_name_set)
{
  Ctx *c = ctx_of (s);
  char *name;

  OPEN ();
  mb_check (arg_string (s, l, &name));
  CLOSE ();
  if (*name != '\0')
    w42_view_set_font_family (c->view, name);
  g_free (name);
  return MB_FUNC_OK;
}

/* Word's colours are BGR longs; the model's are RGB. */
static guint32
rgb_from_vba (int v)
{
  if (v < 0)
    return 0;                           /* wdColorAutomatic: black */
  return ((guint32) (v & 0xFF) << 16) | ((guint32) (v >> 8) & 0xFF) << 8 | ((guint32) (v >> 16) & 0xFF);
}

static int
vba_from_rgb (guint32 rgb)
{
  return (int) (((rgb & 0xFF) << 16) | (rgb & 0xFF00) | ((rgb >> 16) & 0xFF));
}

NATIVE (n_font_color)
{
  Ctx *c = ctx_of (s);
  W42CharFmt ch;

  OPEN (); CLOSE ();
  w42_view_get_char_fmt (c->view, &ch);
  return mb_push_int (s, l, vba_from_rgb (ch.color));
}

NATIVE (n_font_color_set)
{
  Ctx *c = ctx_of (s);
  W42CharFmt ch;
  int v;

  OPEN ();
  mb_check (arg_int (s, l, &v, 0));
  CLOSE ();
  w42_view_get_char_fmt (c->view, &ch);
  ch.color = rgb_from_vba (v);
  w42_view_apply_char_fmt (c->view, W42_CHAR_COLOR, &ch);
  return MB_FUNC_OK;
}

/* ---- ParagraphFormat ---------------------------------------------------- */

/* The paragraph formatting of the selection's first paragraph, which is
 * what Word reports for a selection over several.  The view's own
 * getters read the paragraph the caret is in, and the caret is at
 * whichever end of the selection moved: after MoveDown with wdExtend,
 * the paragraph below. */
static void
sel_para_fmt (Ctx *c, W42ParaFmt *out)
{
  W42PieceTable *pt = pt_of (c);
  gsize start, end;

  w42_view_get_selection_bounds (c->view, &start, &end);
  /* A position just before a paragraph mark is the end of the paragraph
   * before it, as it is for the caret. */
  if (start > 0 && w42_pt_is_block_mark (pt, start))
    start--;
  *out = w42_ap_table_get (w42_pt_ap_table (pt), w42_pt_block_ap_at (pt, start))->pa;
}

NATIVE (n_para_alignment)
{
  Ctx *c = ctx_of (s);
  W42ParaFmt pa;

  OPEN (); CLOSE ();
  sel_para_fmt (c, &pa);
  return mb_push_int (s, l, (int) pa.align);
}

NATIVE (n_para_alignment_set)
{
  Ctx *c = ctx_of (s);
  int v;

  OPEN ();
  mb_check (arg_int (s, l, &v, 0));
  CLOSE ();
  w42_view_set_align (c->view, (W42Align) CLAMP (v, 0, 3));
  return MB_FUNC_OK;
}

/* The indents and spacing, in points. */
static int
para_get_points (struct mb_interpreter_t *s, void **l, int which)
{
  Ctx *c = ctx_of (s);
  W42ParaFmt pa;
  int twips = 0;

  OPEN (); CLOSE ();
  sel_para_fmt (c, &pa);
  switch (which)
    {
    case 0: twips = pa.indent_left; break;
    case 1: twips = pa.indent_right; break;
    case 2: twips = pa.indent_first; break;
    case 3: twips = pa.space_before; break;
    case 4: twips = pa.space_after; break;
    default: break;
    }
  return mb_push_real (s, l, twips / 20.0);
}

static int
para_set_points (struct mb_interpreter_t *s, void **l, int which)
{
  Ctx *c = ctx_of (s);
  W42ParaFmt pa;
  double points;
  int twips;
  W42ParaMask mask = 0;

  OPEN ();
  mb_check (arg_real (s, l, &points, 0.0));
  CLOSE ();
  twips = (int) lround (points * 20.0);
  w42_view_get_para_fmt (c->view, &pa);
  switch (which)
    {
    case 0: pa.indent_left = twips; mask = W42_PARA_INDENT_LEFT; break;
    case 1: pa.indent_right = twips; mask = W42_PARA_INDENT_RIGHT; break;
    case 2: pa.indent_first = twips; mask = W42_PARA_INDENT_FIRST; break;
    case 3: pa.space_before = MAX (twips, 0); mask = W42_PARA_SPACE_BEFORE; break;
    case 4: pa.space_after = MAX (twips, 0); mask = W42_PARA_SPACE_AFTER; break;
    default: break;
    }
  if (mask != 0)
    w42_view_apply_para_fmt (c->view, mask, &pa);
  return MB_FUNC_OK;
}

NATIVE (n_para_leftindent)      { return para_get_points (s, l, 0); }
NATIVE (n_para_leftindent_set)  { return para_set_points (s, l, 0); }
NATIVE (n_para_rightindent)     { return para_get_points (s, l, 1); }
NATIVE (n_para_rightindent_set) { return para_set_points (s, l, 1); }
NATIVE (n_para_firstindent)     { return para_get_points (s, l, 2); }
NATIVE (n_para_firstindent_set) { return para_set_points (s, l, 2); }
NATIVE (n_para_spacebefore)     { return para_get_points (s, l, 3); }
NATIVE (n_para_spacebefore_set) { return para_set_points (s, l, 3); }
NATIVE (n_para_spaceafter)      { return para_get_points (s, l, 4); }
NATIVE (n_para_spaceafter_set)  { return para_set_points (s, l, 4); }

/* KeepWithNext on each paragraph the selection touches, as the view
 * would set any paragraph formatting.  The model keeps it with Keep
 * Lines Together and Widow/Orphan Control as one (W42_PARA_FLOW), so
 * each paragraph is set on its own and keeps its other two: set from
 * the first paragraph's, a long paragraph after a heading would be kept
 * on one page because the heading is. */
static void
set_keep_with_next (Ctx *c, gboolean on)
{
  W42PieceTable *pt = pt_of (c);
  W42Document *doc = w42_view_get_document (c->view);
  gsize start, end, mark;
  gboolean changed = FALSE;

  w42_view_get_selection_bounds (c->view, &start, &end);
  if (start == end && start > 0 && w42_pt_is_block_mark (pt, start))
    start--;
  end = MAX (end, start + 1);
  for (mark = w42_pt_paragraph_start (pt, start); mark < end && mark < w42_pt_length (pt);
       mark = w42_pt_paragraph_end (pt, mark))
    {
      W42ParaFmt pa;

      /* The mark after a paragraph can be a cell's or a table's, with
       * the next paragraph's own mark after it. */
      while (mark < end && mark < w42_pt_length (pt) && !w42_pt_is_block_mark (pt, mark))
        mark++;
      if (mark >= end || mark >= w42_pt_length (pt))
        break;
      pa = w42_ap_table_get (w42_pt_ap_table (pt), w42_pt_block_ap_at (pt, mark))->pa;
      if (pa.keep_next == (on ? 1 : 0))
        continue;
      pa.keep_next = on ? 1 : 0;
      w42_pt_apply_para_fmt (pt, mark, 0, W42_PARA_FLOW, &pa);
      changed = TRUE;
    }
  if (changed)
    {
      w42_document_set_modified (doc, TRUE);
      w42_document_touch (doc);
    }
}

/* PageBreakBefore and KeepWithNext: True, False, or wdToggle. */
static int
para_get_switch (struct mb_interpreter_t *s, void **l, int which)
{
  Ctx *c = ctx_of (s);
  W42ParaFmt pa;

  OPEN (); CLOSE ();
  sel_para_fmt (c, &pa);
  return mb_push_int (s, l, which == 0 ? pa.page_break_before : pa.keep_next);
}

static int
para_set_switch (struct mb_interpreter_t *s, void **l, int which)
{
  Ctx *c = ctx_of (s);
  W42ParaFmt pa;
  int value;

  OPEN ();
  mb_check (arg_int (s, l, &value, 1));
  CLOSE ();
  sel_para_fmt (c, &pa);
  if (which == 0)
    {
      pa.page_break_before = switch_value (value, pa.page_break_before);
      w42_view_apply_para_fmt (c->view, W42_PARA_PAGE_BREAK, &pa);
    }
  else
    set_keep_with_next (c, switch_value (value, pa.keep_next));
  return MB_FUNC_OK;
}

NATIVE (n_para_pagebreakbefore)     { return para_get_switch (s, l, 0); }
NATIVE (n_para_pagebreakbefore_set) { return para_set_switch (s, l, 0); }
NATIVE (n_para_keepwithnext)        { return para_get_switch (s, l, 1); }
NATIVE (n_para_keepwithnext_set)    { return para_set_switch (s, l, 1); }

/* ---- Style ------------------------------------------------------------- */

/* The style of the selection's first paragraph, as ParagraphFormat
 * reads it. */
NATIVE (n_sel_style)
{
  Ctx *c = ctx_of (s);
  W42ParaFmt pa;

  OPEN (); CLOSE ();
  sel_para_fmt (c, &pa);
  return push_string (s, l, pa.style != NULL ? pa.style : "Normal");
}

/* By name, or by Word's built-in numbers: wdStyleNormal, wdStyleHeading1. */
NATIVE (n_sel_style_set)
{
  Ctx *c = ctx_of (s);
  mb_value_t v;
  const char *name = NULL;
  char *owned = NULL;

  OPEN ();
  mb_check (arg_value (s, l, &v));
  CLOSE ();
  if (v.type == MB_DT_STRING)
    name = v.value.string;
  else if (v.type == MB_DT_INT)
    {
      switch (v.value.integer)
        {
        case -1: name = "Normal"; break;
        case -2: name = "Heading 1"; break;
        case -3: name = "Heading 2"; break;
        case -4: name = "Heading 3"; break;
        case -63: name = "Title"; break;
        default: break;
        }
    }
  if (name == NULL)
    return fail (s, l, _("There is no such style"));
  if (w42_stylesheet_find (w42_pt_stylesheet (pt_of (c)), name) == NULL)
    {
      /* Translators: %s is the name of a style. */
      owned = g_strdup_printf (_("There is no style named %s"), name);
      {
        int rc = fail (s, l, owned);

        g_free (owned);
        return rc;
      }
    }
  w42_view_apply_style (c->view, name);
  return MB_FUNC_OK;
}

/* ---- Find ---------------------------------------------------------------- */

NATIVE (n_find_clearformatting) { OPEN (); CLOSE (); return MB_FUNC_OK; }

NATIVE (n_find_text_set)
{
  Ctx *c = ctx_of (s);
  char *t;

  OPEN ();
  mb_check (arg_string (s, l, &t));
  CLOSE ();
  g_free (c->find_text);
  c->find_text = t;
  return MB_FUNC_OK;
}

NATIVE (n_find_text) { Ctx *c = ctx_of (s); OPEN (); CLOSE (); return push_string (s, l, c->find_text); }

NATIVE (n_find_replacement_text_set)
{
  Ctx *c = ctx_of (s);
  char *t;

  OPEN ();
  mb_check (arg_string (s, l, &t));
  CLOSE ();
  g_free (c->replace_text);
  c->replace_text = t;
  return MB_FUNC_OK;
}

static int
find_flag_set (struct mb_interpreter_t *s, void **l, gboolean *flag)
{
  gboolean v;

  OPEN ();
  mb_check (arg_bool (s, l, &v, FALSE));
  CLOSE ();
  *flag = v;
  return MB_FUNC_OK;
}

NATIVE (n_find_matchcase_set)      { return find_flag_set (s, l, &ctx_of (s)->match_case); }
NATIVE (n_find_matchwholeword_set) { return find_flag_set (s, l, &ctx_of (s)->whole_word); }
NATIVE (n_find_forward_set)        { return find_flag_set (s, l, &ctx_of (s)->forward); }
NATIVE (n_find_wrap_set)
{
  Ctx *c = ctx_of (s);
  int v;

  OPEN ();
  mb_check (arg_int (s, l, &v, 1));
  CLOSE ();
  c->wrap = v != 0;
  return MB_FUNC_OK;
}

/* Whether a run of replacements has come back round to where it began,
 * now that a search from `from` has found a match at `start`: back past
 * the place it began, or past the end of the document a second time.
 * A search that goes past the end is noted. */
static gboolean
replace_round (Ctx *c, gsize start, gsize from, gboolean forward)
{
  gboolean wraps = forward ? start < from : start >= from;
  gboolean beyond = forward ? start >= c->replace_origin : start < c->replace_origin;
  gboolean wrapped = c->replace_wrapped;

  c->replace_wrapped = wrapped || wraps;
  return (wrapped && wraps) || (c->replace_wrapped && beyond);
}

/* Replace:=wdReplaceOne: the selection, when it is a match, or else the
 * next match, is replaced, and the match after it selected, as Word's
 * Replace button does.  A macro calls this in a loop until it is False,
 * so the loop has to end even with Wrap on and a replacement that is a
 * match itself: once the search comes back round to where the run
 * began, what is left is what the run put there, and it stops. */
static gboolean
replace_one (Ctx *c, const char *text, const char *with,
             const W42SearchOptions *opts)
{
  W42PieceTable *pt = pt_of (c);
  W42SearchOptions exact = *opts;
  gboolean forward = !opts->backwards;
  gboolean fresh = FALSE;
  gsize s0, e0, start, end, from, after, length;

  w42_view_get_selection_bounds (c->view, &s0, &e0);
  if (c->replace_view != c->view || s0 != c->replace_start || e0 != c->replace_end)
    {
      fresh = TRUE;
      c->replace_view = c->view;
      c->replace_origin = forward ? s0 : e0;
      c->replace_wrapped = FALSE;
      c->replace_done = FALSE;
    }
  if (c->replace_done)
    {
      c->replace_view = NULL;
      return FALSE;
    }

  exact.backwards = FALSE;
  exact.wrap = FALSE;
  if (s0 < e0 && w42_search_find (pt, s0, text, &exact, &start, &end) &&
      start == s0 && end == e0)
    ;                                   /* the selection is the match */
  else
    {
      from = forward ? e0 : s0;
      if (!w42_search_find (pt, from, text, opts, &start, &end))
        {
          c->replace_view = NULL;
          return FALSE;
        }
      if (replace_round (c, start, from, forward) && !fresh)
        {
          c->replace_view = NULL;
          return FALSE;
        }
    }

  /* Replacing with nothing is a deletion, which inserting nothing is
   * not: the match would stay, and be found again. */
  length = w42_pt_length (pt);
  w42_view_select_range (c->view, start, end);
  if (*with == '\0')
    w42_view_clear (c->view);
  else
    w42_view_insert_text (c->view, with);
  after = w42_view_get_caret (c->view);
  if (start < c->replace_origin)
    c->replace_origin = c->replace_origin >= end
                        ? c->replace_origin - (end - start) + (after - start)
                        : after;

  /* And on to the next, unless that is back where the run began -- or
   * the match did not go: marking changes, a deletion only strikes the
   * text through, and it would be found and replaced for ever. */
  from = forward ? after : start;
  if (w42_pt_length (pt) + (end - start) == length + (after - start) &&
      w42_search_find (pt, from, text, opts, &s0, &e0) &&
      !replace_round (c, s0, from, forward))
    w42_view_select_range (c->view, s0, e0);
  else
    c->replace_done = TRUE;
  w42_view_get_selection_bounds (c->view, &c->replace_start, &c->replace_end);
  return TRUE;
}

/* Execute(FindText, MatchCase, MatchWholeWord, MatchWildcards,
 * MatchSoundsLike, MatchAllWordForms, Forward, Wrap, Format, ReplaceWith,
 * Replace): finds and selects the next match, or replaces one or all.
 * TRUE when something was found. */
NATIVE (n_find_execute)
{
  Ctx *c = ctx_of (s);
  W42PieceTable *pt = pt_of (c);
  mb_value_t v;
  char *text = NULL, *with = NULL;
  gboolean match_case, whole_word, forward, has_text = FALSE, has_with = FALSE;
  int wrap, replace;
  W42SearchOptions opts;
  gboolean found = FALSE;

  OPEN ();
  mb_check (arg_value (s, l, &v));
  if (v.type == MB_DT_STRING) { text = g_strdup (v.value.string); has_text = TRUE; }
  mb_check (arg_bool (s, l, &match_case, c->match_case));
  mb_check (arg_bool (s, l, &whole_word, c->whole_word));
  mb_check (arg_value (s, l, &v));      /* MatchWildcards: not done */
  mb_check (arg_value (s, l, &v));      /* MatchSoundsLike */
  mb_check (arg_value (s, l, &v));      /* MatchAllWordForms */
  mb_check (arg_bool (s, l, &forward, c->forward));
  mb_check (arg_int (s, l, &wrap, c->wrap ? 1 : 0));
  mb_check (arg_value (s, l, &v));      /* Format */
  mb_check (arg_value (s, l, &v));
  if (v.type == MB_DT_STRING) { with = g_strdup (v.value.string); has_with = TRUE; }
  mb_check (arg_int (s, l, &replace, 0));
  CLOSE ();

  if (!has_text)
    text = g_strdup (c->find_text != NULL ? c->find_text : "");
  if (!has_with)
    with = g_strdup (c->replace_text != NULL ? c->replace_text : "");
  if (*text == '\0')
    {
      g_free (text); g_free (with);
      return mb_push_int (s, l, 0);
    }

  opts.match_case = match_case;
  opts.whole_word = whole_word;
  opts.backwards = !forward;
  opts.wrap = wrap != 0;

  if (replace == 1)
    found = replace_one (c, text, with, &opts);
  else if (replace == 2)
    {
      gsize n = w42_search_replace_all (pt, text, with, &opts);

      c->replace_view = NULL;
      found = n > 0;
      if (found)
        {
          W42Document *doc = w42_view_get_document (c->view);

          w42_document_set_modified (doc, TRUE);
          w42_document_touch (doc);
        }
    }
  else
    {
      gsize start, end, from;
      gsize s0, e0;

      c->replace_view = NULL;
      w42_view_get_selection_bounds (c->view, &s0, &e0);
      from = forward ? e0 : s0;
      if (w42_search_find (pt, from, text, &opts, &start, &end))
        {
          found = TRUE;
          w42_view_select_range (c->view, start, end);
        }
    }
  g_free (text);
  g_free (with);
  return mb_push_int (s, l, found);
}

/* ---------------------------------------------------------------------- */
/* ActiveDocument                                                          */
/* ---------------------------------------------------------------------- */

static GFile *
doc_file (Ctx *c)
{
  return w42_document_get_file (w42_view_get_document (c->view));
}

NATIVE (n_doc_name)
{
  Ctx *c = ctx_of (s);
  char *title;
  int rc;

  OPEN (); CLOSE ();
  title = w42_document_get_title (w42_view_get_document (c->view));
  rc = push_string (s, l, title);
  g_free (title);
  return rc;
}

NATIVE (n_doc_fullname)
{
  Ctx *c = ctx_of (s);
  GFile *file;
  char *path;
  int rc;

  OPEN (); CLOSE ();
  file = doc_file (c);
  path = file != NULL ? g_file_get_path (file) : NULL;
  if (path == NULL)
    path = w42_document_get_title (w42_view_get_document (c->view));
  rc = push_string (s, l, path);
  g_free (path);
  return rc;
}

NATIVE (n_doc_path)
{
  Ctx *c = ctx_of (s);
  GFile *file;
  char *path = NULL;
  int rc;

  OPEN (); CLOSE ();
  file = doc_file (c);
  if (file != NULL)
    {
      GFile *dir = g_file_get_parent (file);

      path = dir != NULL ? g_file_get_path (dir) : NULL;
      g_clear_object (&dir);
    }
  rc = push_string (s, l, path != NULL ? path : "");
  g_free (path);
  return rc;
}

NATIVE (n_doc_saved)
{
  Ctx *c = ctx_of (s);

  OPEN (); CLOSE ();
  return mb_push_int (s, l, !w42_document_get_modified (w42_view_get_document (c->view)));
}

NATIVE (n_doc_save)
{
  Ctx *c = ctx_of (s);
  GFile *file;
  GError *error = NULL;

  OPEN (); CLOSE ();
  file = doc_file (c);
  if (file == NULL || !W42_IS_WINDOW (c->parent))
    /* Translators: ActiveDocument.SaveAs is macro code: keep it in
     * English. */
    return fail (s, l, _("The document has never been saved: use ActiveDocument.SaveAs \"name\""));
  /* As File > Save: written back to a .doc, a PDF, a web page or a
   * presentation, the file it was read from would be replaced by
   * Word42's rendering of it. */
  if (!w42_io_format_round_trips (file))
    /* Translators: ActiveDocument.SaveAs is macro code: keep it in
     * English. */
    return fail (s, l, _("The document came from a format Word42 does not write "
                         "back as it was: use ActiveDocument.SaveAs \"name\""));
  if (!w42_window_save_to (W42_WINDOW (c->parent), file, &error))
    {
      int rc = fail (s, l, error != NULL ? error->message : _("The document could not be saved"));

      g_clear_error (&error);
      return rc;
    }
  return MB_FUNC_OK;
}

/* SaveAs(FileName[, FileFormat]): the extension decides the format, and
 * a name without one is Rich Text, as File > Save As has it. */
NATIVE (n_doc_saveas)
{
  Ctx *c = ctx_of (s);
  char *name;
  int format;
  GFile *file;
  GError *error = NULL;
  gboolean ok;

  OPEN ();
  mb_check (arg_string (s, l, &name));
  mb_check (arg_int (s, l, &format, 0));
  CLOSE ();
  if (*name == '\0' || !W42_IS_WINDOW (c->parent))
    {
      g_free (name);
      /* Translators: "SaveAs" is a name in the macro language: keep it in
       * English. */
      return fail (s, l, _("SaveAs needs a file name"));
    }
  {
    char *base = g_path_get_basename (name);

    /* A dot does not make an extension: "Mr. Smith" is a name. */
    if (!w42_window_name_has_extension (base))
      {
        const char *ext = format == 16 || format == 12 ? ".docx" : format == 23 ? ".odt"
                        : format == 8 ? ".html" : format == 17 ? ".pdf" : format == 2 ? ".txt" : ".rtf";
        char *with = g_strconcat (name, ext, NULL);

        g_free (name);
        name = with;
      }
    g_free (base);
  }
  file = g_path_is_absolute (name) ? g_file_new_for_path (name)
       : g_file_new_build_filename (g_get_home_dir (), name, NULL);
  ok = w42_window_save_to (W42_WINDOW (c->parent), file, &error);
  g_object_unref (file);
  g_free (name);
  if (!ok)
    {
      int rc = fail (s, l, error != NULL ? error->message : _("The document could not be saved"));

      g_clear_error (&error);
      return rc;
    }
  return MB_FUNC_OK;
}

/* Closing a window frees its view and its document, and the macro is
 * still running on them, so the window goes once the macro has
 * finished: from the main loop, a moment later. */
static gboolean
close_later (gpointer data)
{
  GtkWindow *window = data;

  if (W42_IS_WINDOW (window) && g_object_get_data (G_OBJECT (window), "w42-discard") != NULL)
    w42_window_close_discarding (W42_WINDOW (window));
  else
    gtk_window_close (window);
  return G_SOURCE_REMOVE;
}

/* Close([SaveChanges]): wdSaveChanges saves first, and fails if it
 * cannot; wdDoNotSaveChanges closes without asking; the default asks. */
NATIVE (n_doc_close)
{
  Ctx *c = ctx_of (s);
  int save;

  OPEN ();
  mb_check (arg_int (s, l, &save, -2));
  CLOSE ();
  if (!GTK_IS_WINDOW (c->parent))
    return MB_FUNC_OK;

  if (save == -1)
    {
      GFile *file = doc_file (c);
      GError *error = NULL;

      if (file == NULL || !W42_IS_WINDOW (c->parent) || !w42_io_format_round_trips (file))
        /* Translators: ActiveDocument.SaveAs is macro code: keep it in
         * English. */
        return fail (s, l, _("The document has no file to be saved to: use "
                             "ActiveDocument.SaveAs \"name\" first"));
      if (!w42_window_save_to (W42_WINDOW (c->parent), file, &error))
        {
          int rc = fail (s, l, error != NULL ? error->message : _("The document could not be saved"));

          g_clear_error (&error);
          return rc;
        }
    }
  if (save == 0)
    g_object_set_data (G_OBJECT (c->parent), "w42-discard", GINT_TO_POINTER (1));
  g_idle_add_full (G_PRIORITY_DEFAULT, close_later, g_object_ref (c->parent),
                   g_object_unref);
  return MB_FUNC_OK;
}

/* The whole text: struxes become line ends. */
NATIVE (n_doc_text)
{
  Ctx *c = ctx_of (s);
  W42PieceTable *pt = pt_of (c);
  gsize first;
  char *text;
  int rc;

  OPEN (); CLOSE ();
  first = w42_pt_first_caret_pos (pt);
  text = w42_pt_get_text (pt, first, w42_pt_length (pt) - first);
  rc = push_string (s, l, text);
  g_free (text);
  return rc;
}

NATIVE (n_doc_text_set)
{
  Ctx *c = ctx_of (s);
  char *text;

  OPEN ();
  mb_check (arg_string (s, l, &text));
  CLOSE ();
  w42_view_select_all (c->view);
  if (*text == '\0')
    w42_view_clear (c->view);
  else
    type_text (c, text);
  g_free (text);
  return MB_FUNC_OK;
}

/* ComputeStatistics' numbers: words, lines, pages, characters,
 * paragraphs, characters with spaces. */
static int
push_stat (struct mb_interpreter_t *s, void **l, Ctx *c, int which)
{
  W42Stats st;

  w42_pt_statistics (pt_of (c), TRUE, &st);
  switch (which)
    {
    case 0: return mb_push_int (s, l, (int) st.words);
    case 1: return mb_push_int (s, l, w42_view_line_count (c->view));
    case 2: return mb_push_int (s, l, MAX (w42_layout_n_pages (w42_view_get_layout (c->view)), 1));
    case 3: return mb_push_int (s, l, (int) st.characters_no_spaces);
    case 4: return mb_push_int (s, l, (int) st.paragraphs);
    case 5: return mb_push_int (s, l, (int) st.characters);
    default: return mb_push_int (s, l, 0);
    }
}

static int
doc_stat (struct mb_interpreter_t *s, void **l, int which)
{
  OPEN (); CLOSE ();
  return push_stat (s, l, ctx_of (s), which);
}

NATIVE (n_doc_words_count)      { return doc_stat (s, l, 0); }
NATIVE (n_doc_paragraphs_count) { return doc_stat (s, l, 4); }
NATIVE (n_doc_characters_count) { return doc_stat (s, l, 5); }
NATIVE (n_doc_pages_count)      { return doc_stat (s, l, 2); }

NATIVE (n_doc_computestatistics)
{
  int which;

  OPEN ();
  mb_check (arg_int (s, l, &which, 0));
  CLOSE ();
  return push_stat (s, l, ctx_of (s), which);
}

NATIVE (n_doc_tables_count)
{
  Ctx *c = ctx_of (s);
  W42PieceTable *pt = pt_of (c);
  int n = 0;

  OPEN (); CLOSE ();
  for (int t = 0; t < 4096; t++)
    {
      gsize a, b;

      if (w42_pt_table_bounds (pt, t, &a, &b))
        n++;
    }
  return mb_push_int (s, l, n);
}

NATIVE (n_doc_undo)  { Ctx *c = ctx_of (s); OPEN (); CLOSE (); w42_view_undo (c->view); return MB_FUNC_OK; }
NATIVE (n_doc_redo)  { Ctx *c = ctx_of (s); OPEN (); CLOSE (); w42_view_redo (c->view); return MB_FUNC_OK; }
NATIVE (n_doc_select) { Ctx *c = ctx_of (s); OPEN (); CLOSE (); w42_view_select_all (c->view); return MB_FUNC_OK; }

NATIVE (n_doc_printout)
{
  Ctx *c = ctx_of (s);

  OPEN ();
  while (mb_has_arg (s, l))
    {
      mb_value_t v;

      mb_check (arg_value (s, l, &v));
    }
  CLOSE ();
  if (GTK_IS_WINDOW (c->parent))
    g_action_group_activate_action (G_ACTION_GROUP (c->parent), "print", NULL);
  return MB_FUNC_OK;
}

/* ---------------------------------------------------------------------- */
/* Application and Documents                                               */
/* ---------------------------------------------------------------------- */

NATIVE (n_app_version) { OPEN (); CLOSE (); return push_string (s, l, W42_VERSION); }
NATIVE (n_app_name)    { OPEN (); CLOSE (); return push_string (s, l, "Word42"); }
NATIVE (n_app_visible) { OPEN (); CLOSE (); return mb_push_int (s, l, 1); }
NATIVE (n_app_noop_set)
{
  mb_value_t v;

  OPEN ();
  mb_check (arg_value (s, l, &v));
  CLOSE ();
  return MB_FUNC_OK;
}

NATIVE (n_app_statusbar_set)
{
  Ctx *c = ctx_of (s);
  char *text;

  OPEN ();
  mb_check (arg_string (s, l, &text));
  CLOSE ();
  if (W42_IS_WINDOW (c->parent))
    w42_window_flash_status (W42_WINDOW (c->parent), text);
  g_free (text);
  return MB_FUNC_OK;
}

static gboolean
quit_later (gpointer data)
{
  g_action_group_activate_action (G_ACTION_GROUP (data), "quit", NULL);
  return G_SOURCE_REMOVE;
}

NATIVE (n_app_quit)
{
  Ctx *c = ctx_of (s);
  GtkApplication *app;

  OPEN ();
  while (mb_has_arg (s, l))
    {
      mb_value_t v;

      mb_check (arg_value (s, l, &v));
    }
  CLOSE ();
  /* Once the macro has finished, as ActiveDocument.Close does. */
  app = GTK_IS_WINDOW (c->parent) ? gtk_window_get_application (c->parent) : NULL;
  if (app != NULL)
    g_idle_add_full (G_PRIORITY_DEFAULT, quit_later, g_object_ref (app),
                     g_object_unref);
  return MB_FUNC_OK;
}

NATIVE (n_app_activewindow_caption)
{
  Ctx *c = ctx_of (s);

  OPEN (); CLOSE ();
  return push_string (s, l, GTK_IS_WINDOW (c->parent) ? gtk_window_get_title (c->parent) : "");
}

NATIVE (n_documents_count)
{
  Ctx *c = ctx_of (s);
  GtkApplication *app = GTK_IS_WINDOW (c->parent) ? gtk_window_get_application (c->parent) : NULL;
  int n = 0;

  OPEN (); CLOSE ();
  for (GList *w = app != NULL ? gtk_application_get_windows (app) : NULL; w != NULL; w = w->next)
    if (W42_IS_WINDOW (w->data))
      n++;
  return mb_push_int (s, l, n);
}

NATIVE (n_documents_add)
{
  Ctx *c = ctx_of (s);
  W42Document *doc;
  GtkApplication *app;

  OPEN ();
  while (mb_has_arg (s, l))
    {
      mb_value_t v;

      mb_check (arg_value (s, l, &v));
    }
  CLOSE ();
  doc = w42_window_new_document (c->parent);
  if (doc == NULL)
    return fail (s, l, _("A new document could not be made"));
  /* The new document is the active one: Selection and ActiveDocument
   * go on in its window. */
  app = gtk_window_get_application (c->parent);
  for (GList *w = gtk_application_get_windows (app); w != NULL; w = w->next)
    {
      W42View *view = W42_IS_WINDOW (w->data) ? window_view (w->data) : NULL;

      if (view != NULL && w42_view_get_document (view) == doc)
        {
          ctx_enter (c, w->data, view);
          break;
        }
    }
  return MB_FUNC_OK;
}

/* Documents.Open(FileName): in a window of its own. */
NATIVE (n_documents_open)
{
  Ctx *c = ctx_of (s);
  char *name;
  GtkApplication *app;
  GFile *file;
  GtkWidget *window;
  W42View *view;
  GError *error = NULL;

  OPEN ();
  mb_check (arg_string (s, l, &name));
  CLOSE ();
  app = GTK_IS_WINDOW (c->parent) ? gtk_window_get_application (c->parent) : NULL;
  if (*name == '\0' || app == NULL)
    {
      g_free (name);
      /* Translators: "Open" is Documents.Open, a name in the macro
       * language: keep it in English. */
      return fail (s, l, _("Open needs a file name"));
    }
  file = g_path_is_absolute (name) ? g_file_new_for_path (name)
       : g_file_new_build_filename (g_get_home_dir (), name, NULL);
  if (!g_file_query_exists (file, NULL))
    {
      /* Translators: %s is a file name. */
      char *msg = g_strdup_printf (_("There is no file %s"), name);
      int rc = fail (s, l, msg);

      g_free (msg);
      g_free (name);
      g_object_unref (file);
      return rc;
    }
  window = w42_window_new (app);
  if (!w42_window_load (W42_WINDOW (window), file, &error))
    {
      int rc = fail (s, l, error != NULL ? error->message : _("The file could not be opened"));

      gtk_window_destroy (GTK_WINDOW (window));
      g_clear_error (&error);
      g_object_unref (file);
      g_free (name);
      return rc;
    }
  gtk_window_present (GTK_WINDOW (window));
  g_object_unref (file);
  g_free (name);
  /* As Documents.Add: what follows is done to the document opened. */
  view = window_view (window);
  if (view != NULL)
    ctx_enter (c, GTK_WINDOW (window), view);
  return MB_FUNC_OK;
}

/* ---------------------------------------------------------------------- */
/* Registration                                                            */
/* ---------------------------------------------------------------------- */

typedef struct {
  const char *name;
  mb_func_t   func;
} Native;

static const Native NATIVES[] = {
  /* Globals. */
  { "MSGBOX", n_msgbox }, { "INPUTBOX", n_inputbox }, { "DEBUG_PRINT", n_debug_print },
  { "CAT", n_cat }, { "IDIV", n_idiv }, { "XOR_", n_xor },
  { "MID", n_mid }, { "LEFT", n_left }, { "RIGHT", n_right }, { "LEN", n_len },
  { "UCASE", n_ucase }, { "LCASE", n_lcase }, { "TRIM", n_trim }, { "LTRIM", n_ltrim },
  { "RTRIM", n_rtrim }, { "STRREVERSE", n_strreverse }, { "CSTR", n_cstr },
  { "INSTR", n_instr }, { "REPLACE", n_replace }, { "SPACE", n_space }, { "STRING", n_string },
  { "CINT", n_cint }, { "CLNG", n_cint }, { "INT", n_int }, { "CDBL", n_cdbl }, { "CSNG", n_cdbl },
  { "ISNUMERIC", n_isnumeric }, { "IIF", n_iif }, { "HEX", n_hex }, { "FORMAT", n_format },
  { "NOW", n_now }, { "DATE", n_date }, { "TIME", n_time }, { "YEAR", n_year },
  { "MONTH", n_month }, { "DAY", n_day }, { "TIMER", n_timer },
  /* Selection. */
  { "SELECTION_TYPETEXT", n_sel_typetext }, { "SELECTION_TYPEPARAGRAPH", n_sel_typeparagraph },
  { "SELECTION_TYPEBACKSPACE", n_sel_typebackspace }, { "SELECTION_INSERTPARAGRAPHAFTER", n_sel_typeparagraph },
  { "SELECTION_DELETE", n_sel_delete },
  { "SELECTION_MOVERIGHT", n_sel_moveright }, { "SELECTION_MOVELEFT", n_sel_moveleft },
  { "SELECTION_MOVEDOWN", n_sel_movedown }, { "SELECTION_MOVEUP", n_sel_moveup },
  { "SELECTION_HOMEKEY", n_sel_homekey }, { "SELECTION_ENDKEY", n_sel_endkey },
  { "SELECTION_COLLAPSE", n_sel_collapse }, { "SELECTION_WHOLESTORY", n_sel_wholestory },
  { "SELECTION_SELECTALL", n_sel_wholestory },
  { "SELECTION_TEXT", n_sel_text }, { "SELECTION_TEXT_SET", n_sel_text_set },
  { "SELECTION_RANGE_TEXT", n_sel_text }, { "SELECTION_RANGE_TEXT_SET", n_sel_text_set },
  { "SELECTION_START", n_sel_start }, { "SELECTION_START_SET", n_sel_start_set },
  { "SELECTION_END", n_sel_end }, { "SELECTION_END_SET", n_sel_end_set },
  { "SELECTION_RANGE_START", n_sel_start }, { "SELECTION_RANGE_END", n_sel_end },
  { "SELECTION_COPY", n_sel_copy }, { "SELECTION_CUT", n_sel_cut }, { "SELECTION_PASTE", n_sel_paste },
  { "SELECTION_INSERTAFTER", n_sel_insertafter }, { "SELECTION_INSERTBEFORE", n_sel_insertbefore },
  { "SELECTION_INSERTBREAK", n_sel_insertbreak },
  { "SELECTION_WORDS_COUNT", n_sel_words_count }, { "SELECTION_CHARACTERS_COUNT", n_sel_characters_count },
  { "SELECTION_PARAGRAPHS_COUNT", n_sel_paragraphs_count },
  { "SELECTION_STYLE", n_sel_style }, { "SELECTION_STYLE_SET", n_sel_style_set },
  /* Selection.Font. */
  { "SELECTION_FONT_BOLD", n_font_bold }, { "SELECTION_FONT_BOLD_SET", n_font_bold_set },
  { "SELECTION_FONT_ITALIC", n_font_italic }, { "SELECTION_FONT_ITALIC_SET", n_font_italic_set },
  { "SELECTION_FONT_UNDERLINE", n_font_underline }, { "SELECTION_FONT_UNDERLINE_SET", n_font_underline_set },
  { "SELECTION_FONT_STRIKETHROUGH", n_font_strike }, { "SELECTION_FONT_STRIKETHROUGH_SET", n_font_strike_set },
  { "SELECTION_FONT_SUPERSCRIPT", n_font_super }, { "SELECTION_FONT_SUPERSCRIPT_SET", n_font_super_set },
  { "SELECTION_FONT_SUBSCRIPT", n_font_sub }, { "SELECTION_FONT_SUBSCRIPT_SET", n_font_sub_set },
  { "SELECTION_FONT_ALLCAPS", n_font_allcaps }, { "SELECTION_FONT_ALLCAPS_SET", n_font_allcaps_set },
  { "SELECTION_FONT_SMALLCAPS", n_font_smallcaps }, { "SELECTION_FONT_SMALLCAPS_SET", n_font_smallcaps_set },
  { "SELECTION_FONT_DOUBLESTRIKETHROUGH", n_font_dstrike }, { "SELECTION_FONT_DOUBLESTRIKETHROUGH_SET", n_font_dstrike_set },
  { "SELECTION_FONT_SHADOW", n_font_shadow }, { "SELECTION_FONT_SHADOW_SET", n_font_shadow_set },
  { "SELECTION_FONT_OUTLINE", n_font_outline }, { "SELECTION_FONT_OUTLINE_SET", n_font_outline_set },
  { "SELECTION_FONT_EMBOSS", n_font_emboss }, { "SELECTION_FONT_EMBOSS_SET", n_font_emboss_set },
  { "SELECTION_FONT_ENGRAVE", n_font_engrave }, { "SELECTION_FONT_ENGRAVE_SET", n_font_engrave_set },
  { "SELECTION_FONT_SIZE", n_font_size }, { "SELECTION_FONT_SIZE_SET", n_font_size_set },
  { "SELECTION_FONT_NAME", n_font_name }, { "SELECTION_FONT_NAME_SET", n_font_name_set },
  { "SELECTION_FONT_COLOR", n_font_color }, { "SELECTION_FONT_COLOR_SET", n_font_color_set },
  /* Selection.ParagraphFormat. */
  { "SELECTION_PARAGRAPHFORMAT_ALIGNMENT", n_para_alignment },
  { "SELECTION_PARAGRAPHFORMAT_ALIGNMENT_SET", n_para_alignment_set },
  { "SELECTION_PARAGRAPHFORMAT_LEFTINDENT", n_para_leftindent },
  { "SELECTION_PARAGRAPHFORMAT_LEFTINDENT_SET", n_para_leftindent_set },
  { "SELECTION_PARAGRAPHFORMAT_RIGHTINDENT", n_para_rightindent },
  { "SELECTION_PARAGRAPHFORMAT_RIGHTINDENT_SET", n_para_rightindent_set },
  { "SELECTION_PARAGRAPHFORMAT_FIRSTLINEINDENT", n_para_firstindent },
  { "SELECTION_PARAGRAPHFORMAT_FIRSTLINEINDENT_SET", n_para_firstindent_set },
  { "SELECTION_PARAGRAPHFORMAT_SPACEBEFORE", n_para_spacebefore },
  { "SELECTION_PARAGRAPHFORMAT_SPACEBEFORE_SET", n_para_spacebefore_set },
  { "SELECTION_PARAGRAPHFORMAT_SPACEAFTER", n_para_spaceafter },
  { "SELECTION_PARAGRAPHFORMAT_SPACEAFTER_SET", n_para_spaceafter_set },
  { "SELECTION_PARAGRAPHFORMAT_PAGEBREAKBEFORE", n_para_pagebreakbefore },
  { "SELECTION_PARAGRAPHFORMAT_PAGEBREAKBEFORE_SET", n_para_pagebreakbefore_set },
  { "SELECTION_PARAGRAPHFORMAT_KEEPWITHNEXT", n_para_keepwithnext },
  { "SELECTION_PARAGRAPHFORMAT_KEEPWITHNEXT_SET", n_para_keepwithnext_set },
  /* Selection.Find. */
  { "SELECTION_FIND_CLEARFORMATTING", n_find_clearformatting },
  { "SELECTION_FIND_REPLACEMENT_CLEARFORMATTING", n_find_clearformatting },
  { "SELECTION_FIND_TEXT", n_find_text }, { "SELECTION_FIND_TEXT_SET", n_find_text_set },
  { "SELECTION_FIND_REPLACEMENT_TEXT_SET", n_find_replacement_text_set },
  { "SELECTION_FIND_MATCHCASE_SET", n_find_matchcase_set },
  { "SELECTION_FIND_MATCHWHOLEWORD_SET", n_find_matchwholeword_set },
  { "SELECTION_FIND_FORWARD_SET", n_find_forward_set },
  { "SELECTION_FIND_WRAP_SET", n_find_wrap_set },
  { "SELECTION_FIND_EXECUTE", n_find_execute },
  /* ActiveDocument. */
  { "ACTIVEDOCUMENT_NAME", n_doc_name }, { "ACTIVEDOCUMENT_FULLNAME", n_doc_fullname },
  { "ACTIVEDOCUMENT_PATH", n_doc_path }, { "ACTIVEDOCUMENT_SAVED", n_doc_saved },
  { "ACTIVEDOCUMENT_SAVE", n_doc_save }, { "ACTIVEDOCUMENT_SAVEAS", n_doc_saveas },
  { "ACTIVEDOCUMENT_SAVEAS2", n_doc_saveas }, { "ACTIVEDOCUMENT_CLOSE", n_doc_close },
  { "ACTIVEDOCUMENT_TEXT", n_doc_text }, { "ACTIVEDOCUMENT_TEXT_SET", n_doc_text_set },
  { "ACTIVEDOCUMENT_WORDS_COUNT", n_doc_words_count },
  { "ACTIVEDOCUMENT_PARAGRAPHS_COUNT", n_doc_paragraphs_count },
  { "ACTIVEDOCUMENT_CHARACTERS_COUNT", n_doc_characters_count },
  { "ACTIVEDOCUMENT_PAGES_COUNT", n_doc_pages_count },
  { "ACTIVEDOCUMENT_TABLES_COUNT", n_doc_tables_count },
  { "ACTIVEDOCUMENT_COMPUTESTATISTICS", n_doc_computestatistics },
  { "ACTIVEDOCUMENT_UNDO", n_doc_undo }, { "ACTIVEDOCUMENT_REDO", n_doc_redo },
  { "ACTIVEDOCUMENT_SELECT", n_doc_select }, { "ACTIVEDOCUMENT_PRINTOUT", n_doc_printout },
  /* Application and Documents. */
  { "APPLICATION_VERSION", n_app_version }, { "APPLICATION_NAME", n_app_name },
  { "APPLICATION_VISIBLE", n_app_visible }, { "APPLICATION_VISIBLE_SET", n_app_noop_set },
  { "APPLICATION_SCREENUPDATING", n_app_visible }, { "APPLICATION_SCREENUPDATING_SET", n_app_noop_set },
  { "APPLICATION_DISPLAYALERTS_SET", n_app_noop_set },
  { "APPLICATION_STATUSBAR_SET", n_app_statusbar_set }, { "APPLICATION_QUIT", n_app_quit },
  { "APPLICATION_ACTIVEWINDOW_CAPTION", n_app_activewindow_caption },
  { "WORD42_VERSION", n_app_version },
  { "DOCUMENTS_COUNT", n_documents_count }, { "DOCUMENTS_ADD", n_documents_add },
  { "DOCUMENTS_OPEN", n_documents_open },
  { NULL, NULL }
};

/* ---------------------------------------------------------------------- */
/* Running                                                                 */
/* ---------------------------------------------------------------------- */

static int
on_print (struct mb_interpreter_t *s, const char *fmt, ...)
{
  Ctx *c = ctx_of (s);
  va_list args;
  char *text;

  va_start (args, fmt);
  text = g_strdup_vprintf (fmt, args);
  va_end (args);
  if (c != NULL && c->output != NULL)
    g_string_append (c->output, text);
  g_free (text);
  return 0;
}

static void
on_error (struct mb_interpreter_t *s, mb_error_e e, const char *m, const char *f,
          int p, unsigned short row, unsigned short col, int abort_code)
{
  Ctx *c = ctx_of (s);

  (void) f; (void) p; (void) col; (void) abort_code;
  if (e == SE_NO_ERR || c == NULL)
    return;
  if (c->error == NULL)
    c->error = g_strdup (m != NULL ? m : _("Error"));
  if (c->error_row == 0)
    c->error_row = row;
}

/* Called before every statement: the runaway guard, and a chance for the
 * screen to catch up so a long macro is seen working. */
static int
on_step (struct mb_interpreter_t *s, void **l, const char *f, int p,
         unsigned short row, unsigned short col)
{
  Ctx *c = ctx_of (s);

  (void) f; (void) p; (void) row; (void) col;
  if (c == NULL)
    return MB_FUNC_OK;
  c->steps++;
  if (c->steps > STEP_LIMIT)
    {
      c->runaway = TRUE;
      return fail (s, l, _("The macro ran too long and was stopped"));
    }
  if (c->gone)
    return fail (s, l, _("The document was closed while the macro ran"));
  return MB_FUNC_OK;
}

static void
on_gone (GtkWidget *widget, gpointer data)
{
  Ctx *c = data;

  /* A window the macro has moved on from may go; only the one it works
   * in now stops it. */
  if (widget != (GtkWidget *) c->view && widget != (GtkWidget *) c->parent)
    return;
  c->gone = TRUE;
  /* A box being answered is answered: nobody is left to answer it. */
  if (c->modal != NULL && c->modal->loop != NULL)
    g_main_loop_quit (c->modal->loop);
}

gboolean
w42_macro_run (GtkWindow *parent, W42View *view, const char *source,
               const char *entry, GString *output, char **error)
{
  /* MY-BASIC keeps some of its state in globals that mb_dispose() frees,
   * and a MsgBox runs the main loop: a second macro started from it --
   * F5 in the editor, or Tools > Macro in another window -- would free
   * them under the first. */
  static gboolean running;
  W42VbaProgram *prog;
  struct mb_interpreter_t *bas = NULL;
  Ctx ctx;
  int rc;
  gboolean ok = TRUE;
  W42Document *doc;

  g_return_val_if_fail (W42_IS_VIEW (view), FALSE);
  g_return_val_if_fail (source != NULL, FALSE);
  doc = w42_view_get_document (view);
  g_return_val_if_fail (W42_IS_DOCUMENT (doc), FALSE);
  if (error != NULL)
    *error = NULL;

  if (running)
    {
      if (error != NULL)
        *error = g_strdup (_("A macro is running already: answer its box first."));
      return FALSE;
    }

  prog = w42_vba_translate (source, entry != NULL ? entry : "Main");
  if (prog->error != NULL)
    {
      if (error != NULL)
        *error = g_strdup (prog->error);
      w42_vba_program_free (prog);
      return FALSE;
    }

  memset (&ctx, 0, sizeof ctx);
  ctx.output = output;
  ctx.forward = TRUE;
  ctx.wrap = TRUE;
  ctx.held = g_ptr_array_new ();
  ctx.docs = g_ptr_array_new_with_free_func (g_object_unref);

  running = TRUE;
  ctx_enter (&ctx, parent, view);

  mb_init ();
  mb_open (&bas);
  mb_set_userdata (bas, &ctx);
  mb_set_printer (bas, on_print);
  mb_set_error_handler (bas, on_error);
  mb_debug_set_stepped_handler (bas, on_step, NULL);
  /* INPUT reads the terminal, which a word processor has not got. */
  mb_remove_reserved_func (bas, "INPUT");
  for (int i = 0; NATIVES[i].name != NULL; i++)
    {
      mb_remove_reserved_func (bas, NATIVES[i].name);
      mb_register_func (bas, NATIVES[i].name, NATIVES[i].func);
    }

  w42_pt_break_undo_coalesce (w42_document_pt (doc));
  rc = mb_load_string (bas, prog->program, true);
  if (rc == MB_FUNC_OK)
    {
      /* Everything the macro does is one undo step, in each document it
       * works on.  The table is asked for again at the end: a Revert
       * while the macro waited on a box gives the document another. */
      ctx.grouping = TRUE;
      w42_pt_begin_group (w42_document_pt (doc));
      rc = mb_run (bas, true);
      for (guint i = 0; i < ctx.docs->len; i++)
        w42_pt_end_group (w42_document_pt (g_ptr_array_index (ctx.docs, i)));
      ctx.grouping = FALSE;
    }

  if (rc != MB_FUNC_OK || ctx.error != NULL)
    {
      int line = w42_vba_source_line (prog, ctx.error_row);
      const char *what = ctx.error != NULL ? ctx.error : mb_get_error_desc (mb_get_last_error (bas, NULL, NULL, NULL, NULL));

      if (error != NULL)
        /* Translators: %d is a line of the macro, %s what is wrong with it. */
        *error = line > 0 ? g_strdup_printf (_("Line %d: %s"), line, what)
                          : g_strdup_printf ("%s", what);
      ok = FALSE;
    }

  mb_close (&bas);
  mb_dispose ();
  g_free (ctx.error);
  g_free (ctx.find_text);
  g_free (ctx.replace_text);
  w42_vba_program_free (prog);

  /* Whatever was changed, every view on the document sees it. */
  for (guint i = 0; i < ctx.docs->len; i++)
    w42_document_touch (g_ptr_array_index (ctx.docs, i));

  /* A widget disposed while held has lost its handlers already. */
  for (guint i = 0; i < ctx.held->len; i++)
    {
      gpointer widget = g_ptr_array_index (ctx.held, i);

      g_signal_handlers_disconnect_by_func (widget, on_gone, &ctx);
      g_object_unref (widget);
    }
  g_ptr_array_unref (ctx.held);
  g_ptr_array_unref (ctx.docs);
  running = FALSE;
  return ok;
}

/* ---------------------------------------------------------------------- */
/* The macros folder                                                       */
/* ---------------------------------------------------------------------- */

char *
w42_macro_dir (void)
{
  char *dir = g_build_filename (g_get_user_data_dir (), "word42", "macros", NULL);

  g_mkdir_with_parents (dir, 0700);
  return dir;
}

gboolean
w42_macro_name_ok (const char *name)
{
  if (name == NULL || *name == '\0' || strlen (name) > 64)
    return FALSE;
  if (!g_ascii_isalpha (name[0]) && name[0] != '_')
    return FALSE;
  for (const char *p = name; *p != '\0'; p++)
    if (!g_ascii_isalnum (*p) && *p != '_')
      return FALSE;
  return TRUE;
}

static char *
macro_path (const char *name)
{
  char *dir = w42_macro_dir ();
  char *file = g_strconcat (name, ".bas", NULL);
  char *path = g_build_filename (dir, file, NULL);

  g_free (dir);
  g_free (file);
  return path;
}

static int
name_cmp (gconstpointer a, gconstpointer b)
{
  return g_ascii_strcasecmp (*(const char *const *) a, *(const char *const *) b);
}

char **
w42_macro_names (void)
{
  char *dir = w42_macro_dir ();
  GDir *d = g_dir_open (dir, 0, NULL);
  GPtrArray *names = g_ptr_array_new ();
  const char *entry;

  while (d != NULL && (entry = g_dir_read_name (d)) != NULL)
    {
      if (g_str_has_suffix (entry, ".bas"))
        {
          char *name = g_strndup (entry, strlen (entry) - 4);

          if (w42_macro_name_ok (name))
            g_ptr_array_add (names, name);
          else
            g_free (name);
        }
    }
  if (d != NULL)
    g_dir_close (d);
  g_free (dir);
  g_ptr_array_sort (names, name_cmp);
  g_ptr_array_add (names, NULL);
  return (char **) g_ptr_array_free (names, FALSE);
}

char *
w42_macro_load (const char *name, GError **error)
{
  char *path, *text = NULL;

  g_return_val_if_fail (w42_macro_name_ok (name), NULL);
  path = macro_path (name);
  g_file_get_contents (path, &text, NULL, error);
  g_free (path);
  return text;
}

gboolean
w42_macro_save (const char *name, const char *source, GError **error)
{
  char *path;
  gboolean ok;

  g_return_val_if_fail (w42_macro_name_ok (name), FALSE);
  path = macro_path (name);
  ok = g_file_set_contents (path, source != NULL ? source : "", -1, error);
  g_free (path);
  return ok;
}

gboolean
w42_macro_delete (const char *name, GError **error)
{
  char *path;
  gboolean ok;

  g_return_val_if_fail (w42_macro_name_ok (name), FALSE);
  path = macro_path (name);
  ok = g_unlink (path) == 0;
  if (!ok)
    g_set_error (error, G_FILE_ERROR, g_file_error_from_errno (errno),
                 /* Translators: %s is the macro's name. */
                 _("The macro %s could not be deleted."), name);
  g_free (path);
  return ok;
}
