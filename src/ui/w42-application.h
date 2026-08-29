/* w42-application.h - the GtkApplication for word42
 *
 * Copyright (C) 2026 Andreas Røsdal
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#pragma once

#include <gtk/gtk.h>

G_BEGIN_DECLS

#define W42_TYPE_APPLICATION (w42_application_get_type ())
G_DECLARE_FINAL_TYPE (W42Application, w42_application, W42, APPLICATION, GtkApplication)

W42Application *w42_application_new (void);

G_END_DECLS
