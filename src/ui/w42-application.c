/* w42-application.c - see w42-application.h
 *
 * Copyright (C) 2026 Andreas Røsdal
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "w42-application.h"

#include "w42-settings.h"
#include "w42-help.h"

#include "w42-window.h"

#ifdef G_OS_WIN32
#include <gdk/win32/gdkwin32.h>
#include <windows.h>
#endif

struct _W42Application {
  GtkApplication parent_instance;
};

G_DEFINE_FINAL_TYPE (W42Application, w42_application, GTK_TYPE_APPLICATION)

/* Word 6's shortcuts, as far as they still make sense on a modern desktop. */
static const struct {
  const char *action;
  const char *accels[3];
} ACCELS[] = {
  { "win.new",        { "<Control>n", NULL } },
  { "win.open",       { "<Control>o", NULL } },
  { "win.save",       { "<Control>s", NULL } },
  { "win.save-as",    { "<Control><Shift>s", "F12", NULL } },
  { "win.close",      { "<Control>w", NULL } },
  { "win.undo",       { "<Control>z", NULL } },
  { "win.redo",       { "<Control>y", "<Control><Shift>z", NULL } },
  { "win.cut",        { "<Control>x", NULL } },
  { "win.copy",       { "<Control>c", NULL } },
  { "win.paste",      { "<Control>v", NULL } },
  { "win.select-all", { "<Control>a", NULL } },
  { "win.font",       { "<Control>d", NULL } },
  { "win.insert-break", { "<Control>Return", NULL } },
  { "win.list-bullets", { "<Control><Shift>l", NULL } },
  { "win.apply-style::Normal",    { "<Control><Shift>n", NULL } },
  { "win.apply-style::Heading 1", { "<Control><Alt>1", NULL } },
  { "win.apply-style::Heading 2", { "<Control><Alt>2", NULL } },
  { "win.apply-style::Heading 3", { "<Control><Alt>3", NULL } },
  { "win.print",      { "<Control>p", NULL } },
  { "win.find",       { "<Control>f", NULL } },
  { "win.go-to",      { "<Control>g", "F5", NULL } },
  { "win.help-contents", { "F1", NULL } },
  { "win.insert-date", { "<Alt><Shift>d", NULL } },
  { "win.insert-footnote", { "<Control><Alt>f", NULL } },
  { "win.track-changes",   { "<Control><Shift>e", NULL } },
  { "win.change-case::toggle", { "<Shift>F3", NULL } },
  { "win.show-marks",      { "<Control><Shift>8", "<Control>asterisk", NULL } },
  { "win.bold",            { "<Control>b", NULL } },
  { "win.italic",          { "<Control>i", NULL } },
  { "win.underline",       { "<Control>u", NULL } },
  { "win.align::left",     { "<Control>l", NULL } },
  { "win.align::center",   { "<Control>e", NULL } },
  { "win.align::right",    { "<Control>r", NULL } },
  { "win.align::justify",  { "<Control>j", NULL } },
  { "win.font-grow",       { "<Control>bracketright", NULL } },
  { "win.font-shrink",     { "<Control>bracketleft", NULL } },
  { "win.spelling",        { "F7", NULL } },
  { "win.print-preview",   { "<Control>F2", NULL } },
  { "win.update-fields",   { "F9", NULL } },
  { "win.hyperlink",  { "<Control>k", NULL } },
  { "win.bookmark",   { "<Control><Shift>F5", NULL } },
  { "win.annotation", { "<Control><Alt>a", NULL } },
  { "win.go-to-note", { "<Control><Alt>n", NULL } },
  { "win.insert-endnote", { "<Control><Alt>e", NULL } },
  { "win.replace",    { "<Control>h", NULL } },
  { "win.find-next",  { "F3", NULL } },
  { "win.autotext-expand", { "<Control>F3", NULL } },
  { "app.quit",       { "<Control>q", NULL } },
};

static void
action_quit (GSimpleAction *action, GVariant *param, gpointer data)
{
  GList *windows = g_list_copy (gtk_application_get_windows (GTK_APPLICATION (data)));

  (void) action; (void) param;

  /* Each window gets its own "Save changes?" through close-request; the
   * application ends when the last one has gone. */
  for (GList *l = windows; l != NULL; l = l->next)
    gtk_window_close (GTK_WINDOW (l->data));
  g_list_free (windows);
}

static const GActionEntry APP_ACTIONS[] = {
  { "quit", action_quit, NULL, NULL, NULL, { 0 } },
};

static void
load_css (void)
{
  GtkCssProvider *provider = gtk_css_provider_new ();
  GdkDisplay *display = gdk_display_get_default ();

  gtk_css_provider_load_from_resource (provider, "/org/word42/word42/style.css");

  if (display != NULL)
    gtk_style_context_add_provider_for_display (display,
                                                GTK_STYLE_PROVIDER (provider),
                                                GTK_STYLE_PROVIDER_PRIORITY_APPLICATION);

  g_object_unref (provider);
}

/* word42 draws its own toolbar icons rather than borrowing the desktop's,
 * because a Word 6 toolbar in flat monochrome symbolics would be a different
 * program wearing the same menus.  They live in a GResource laid out as an
 * icon theme, so gtk_icon_theme_add_resource_path() is all it takes to make
 * them resolvable by name. */
static void
load_icons (void)
{
  GdkDisplay *display = gdk_display_get_default ();

  if (display == NULL)
    return;

  gtk_icon_theme_add_resource_path (gtk_icon_theme_get_for_display (display),
                                    "/org/word42/word42/icons");

  gtk_window_set_default_icon_name ("org.word42.word42");
}

/* ---- Splash screen ---------------------------------------------------- */

/* The logo, shown for a moment over the first window, as Word 6 showed
 * its own while it got ready.  A transient window sits centred on its
 * parent, and going away by itself it needs no button. */
#define SPLASH_MS 600

static void
on_splash_destroyed (GtkWidget *splash, gpointer data)
{
  (void) splash;
  g_source_remove (GPOINTER_TO_UINT (data));
}

static gboolean
on_splash_done (gpointer data)
{
  GtkWindow *splash = data;

  g_signal_handlers_disconnect_matched (splash, G_SIGNAL_MATCH_FUNC, 0, 0, NULL, on_splash_destroyed, NULL);
  gtk_window_destroy (splash);
  return G_SOURCE_REMOVE;
}

/* GTK 4 leaves where a window goes to the system, and Windows puts a
 * transient wherever it likes; the splash is moved to the middle of its
 * parent once both have surfaces.  Elsewhere the window manager centres
 * transients itself. */
static void
center_on_parent (GtkWidget *splash, GtkWindow *parent)
{
#ifdef G_OS_WIN32
  GdkSurface *ss = gtk_native_get_surface (GTK_NATIVE (splash));
  GdkSurface *ps = gtk_native_get_surface (GTK_NATIVE (parent));
  HWND sh, ph;
  RECT pr, sr;

  if (ss == NULL || ps == NULL || !GDK_IS_WIN32_SURFACE (ss) || !GDK_IS_WIN32_SURFACE (ps))
    return;
  sh = gdk_win32_surface_get_handle (ss);
  ph = gdk_win32_surface_get_handle (ps);
  if (sh == NULL || ph == NULL || !GetWindowRect (ph, &pr) || !GetWindowRect (sh, &sr))
    return;
  SetWindowPos (sh, HWND_TOP,
                (pr.left + pr.right) / 2 - (sr.right - sr.left) / 2,
                (pr.top + pr.bottom) / 2 - (sr.bottom - sr.top) / 2,
                0, 0, SWP_NOSIZE | SWP_NOACTIVATE);
#else
  (void) splash; (void) parent;
#endif
}

static gboolean
on_splash_mapped (gpointer data)
{
  GtkWidget *splash = data;
  GtkWindow *parent;

  /* The splash may have gone already -- its parent closed at once, say --
   * and the reference below is all that is left of it. */
  if (!gtk_widget_get_visible (splash))
    return G_SOURCE_REMOVE;

  parent = gtk_window_get_transient_for (GTK_WINDOW (splash));
  if (parent != NULL)
    center_on_parent (splash, parent);
  return G_SOURCE_REMOVE;
}

static void
show_splash (GtkWindow *parent)
{
  static gboolean shown;
  GtkWidget *splash, *box, *picture, *label;

  if (shown)
    return;                       /* once per run, not once per document */
  shown = TRUE;

  splash = gtk_window_new ();
  gtk_window_set_transient_for (GTK_WINDOW (splash), parent);
  gtk_window_set_destroy_with_parent (GTK_WINDOW (splash), TRUE);
  gtk_window_set_decorated (GTK_WINDOW (splash), FALSE);
  gtk_window_set_resizable (GTK_WINDOW (splash), FALSE);
  gtk_window_set_title (GTK_WINDOW (splash), "Word42");
  gtk_widget_add_css_class (splash, "w42-splash");

  box = gtk_box_new (GTK_ORIENTATION_VERTICAL, 6);
  gtk_widget_set_margin_top (box, 18);
  gtk_widget_set_margin_bottom (box, 14);
  gtk_widget_set_margin_start (box, 24);
  gtk_widget_set_margin_end (box, 24);

  picture = gtk_picture_new_for_resource ("/org/word42/word42/about.png");
  gtk_picture_set_content_fit (GTK_PICTURE (picture), GTK_CONTENT_FIT_CONTAIN);
  gtk_widget_set_size_request (picture, 420, 128);
  gtk_box_append (GTK_BOX (box), picture);

  label = gtk_label_new ("Word processor  \u00b7  version " W42_VERSION);
  gtk_widget_add_css_class (label, "dim-label");
  gtk_box_append (GTK_BOX (box), label);

  gtk_window_set_child (GTK_WINDOW (splash), box);
  gtk_window_present (GTK_WINDOW (splash));
  g_idle_add_full (G_PRIORITY_DEFAULT_IDLE, on_splash_mapped,
                   g_object_ref (splash), g_object_unref);

  {
    /* Closed early -- with its parent, say -- the timeout must not
     * destroy it a second time. */
    guint id = g_timeout_add (SPLASH_MS, on_splash_done, splash);

    g_signal_connect (splash, "destroy", G_CALLBACK (on_splash_destroyed), GUINT_TO_POINTER (id));
  }
}

static void
w42_application_startup (GApplication *app)
{
  G_APPLICATION_CLASS (w42_application_parent_class)->startup (app);

  w42_settings_load ();

  load_css ();
  load_icons ();

  for (guint i = 0; i < G_N_ELEMENTS (ACCELS); i++)
    gtk_application_set_accels_for_action (GTK_APPLICATION (app),
                                           ACCELS[i].action,
                                           ACCELS[i].accels);
}

/* Word 6 opened with a tip of the day, and so does this -- after the
 * splash has had its moment, and only when no document was named on the
 * command line. */
static gboolean
on_tip_time (gpointer data)
{
  GtkWindow *window = data;

  if (GTK_IS_WINDOW (window))
    w42_tip_of_the_day_show (window, TRUE);
  return G_SOURCE_REMOVE;
}

static void
show_tip_after_splash (GtkWindow *parent)
{
  g_timeout_add_full (G_PRIORITY_DEFAULT_IDLE, SPLASH_MS + 400, on_tip_time,
                      g_object_ref (parent), g_object_unref);
}

static void
w42_application_activate (GApplication *app)
{
  GtkWidget *window;

  /* Documents left behind by a crash come first; with any of those there
   * is no call for an empty one as well. */
  if (w42_window_recover_all (GTK_APPLICATION (app)) > 0)
    {
      GList *windows = gtk_application_get_windows (GTK_APPLICATION (app));

      if (windows != NULL)
        show_splash (GTK_WINDOW (windows->data));
      return;
    }

  window = w42_window_new (GTK_APPLICATION (app));
  gtk_window_present (GTK_WINDOW (window));
  show_splash (GTK_WINDOW (window));
  show_tip_after_splash (GTK_WINDOW (window));
}

static void
w42_application_open (GApplication  *app,
                      GFile        **files,
                      int            n_files,
                      const char    *hint)
{
  (void) hint;

  w42_window_recover_all (GTK_APPLICATION (app));

  for (int i = 0; i < n_files; i++)
    {
      GtkWidget *window = w42_window_new (GTK_APPLICATION (app));

      w42_window_open (W42_WINDOW (window), files[i]);
      gtk_window_present (GTK_WINDOW (window));
      if (i == 0)
        show_splash (GTK_WINDOW (window));
    }
}

static void
w42_application_class_init (W42ApplicationClass *klass)
{
  GApplicationClass *app_class = G_APPLICATION_CLASS (klass);

  app_class->startup  = w42_application_startup;
  app_class->activate = w42_application_activate;
  app_class->open     = w42_application_open;
}

static void
w42_application_init (W42Application *self)
{
  g_action_map_add_action_entries (G_ACTION_MAP (self), APP_ACTIONS,
                                   G_N_ELEMENTS (APP_ACTIONS), self);
}

W42Application *
w42_application_new (void)
{
  return g_object_new (W42_TYPE_APPLICATION,
                       "application-id", "org.word42.word42",
                       "flags", G_APPLICATION_HANDLES_OPEN,
                       NULL);
}
