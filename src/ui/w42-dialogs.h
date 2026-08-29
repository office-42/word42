/* w42-dialogs.h - the Page Setup and Paragraph boxes
 *
 * Copyright (C) 2026 Andreas Røsdal
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * Both are plain form dialogs over settings the document model already
 * carries and nothing in the interface could previously reach.
 */

#pragma once

#include <gtk/gtk.h>

#include "w42-view.h"

G_BEGIN_DECLS

void w42_page_setup_dialog_show (GtkWindow *parent, W42View *view);
void w42_paragraph_dialog_show  (GtkWindow *parent, W42View *view);
void w42_style_dialog_show      (GtkWindow *parent, W42View *view);
void w42_header_footer_dialog_show (GtkWindow *parent, W42View *view);
void w42_page_numbers_dialog_show  (GtkWindow *parent, W42View *view);
void w42_insert_table_dialog_show  (GtkWindow *parent, W42View *view);
void w42_go_to_dialog_show         (GtkWindow *parent, W42View *view);
void w42_autocorrect_dialog_show (GtkWindow *parent, W42View *view);
void w42_tabs_dialog_show          (GtkWindow *parent, W42View *view);
void w42_borders_dialog_show       (GtkWindow *parent, W42View *view);
void w42_hyperlink_dialog_show     (GtkWindow *parent, W42View *view);
void w42_effects_dialog_show       (GtkWindow *parent, W42View *view);
void w42_columns_dialog_show       (GtkWindow *parent, W42View *view);
void w42_bookmark_dialog_show      (GtkWindow *parent, W42View *view);
void w42_annotations_dialog_show   (GtkWindow *parent, W42View *view);
void w42_mail_merge_dialog_show    (GtkWindow *parent, W42View *view);
void w42_cross_reference_dialog_show (GtkWindow *parent, W42View *view);
void w42_drawing_dialog_show       (GtkWindow *parent, W42View *view);
void w42_list_dialog_show          (GtkWindow *parent, W42View *view);
void w42_table_properties_dialog_show (GtkWindow *parent, W42View *view);
void w42_table_autoformat_dialog_show (GtkWindow *parent, W42View *view);
void w42_language_dialog_show (GtkWindow *parent, W42View *view, W42Spell *spell);
void w42_word_count_dialog_show (GtkWindow *parent, W42View *view);
void w42_autotext_dialog_show (GtkWindow *parent, W42View *view);
void w42_background_dialog_show (GtkWindow *parent, W42View *view);
void w42_envelope_dialog_show (GtkWindow *parent, W42View *view);
void w42_template_dialog_show (GtkWindow *parent, W42View *view);
void w42_index_entry_dialog_show (GtkWindow *parent, W42View *view);
void w42_picture_dialog_show (GtkWindow *parent, W42View *view);
void w42_summary_dialog_show (GtkWindow *parent, W42View *view);
void w42_drop_cap_dialog_show (GtkWindow *parent, W42View *view);
void w42_frame_dialog_show (GtkWindow *parent, W42View *view);
void w42_field_dialog_show         (GtkWindow *parent, W42View *view);

/* Tools > Options: units, default view and zoom, spelling as you type. */
void w42_options_dialog_show       (GtkWindow *parent, W42View *view);

void w42_date_time_dialog_show     (GtkWindow *parent, W42View *view);
void w42_symbol_dialog_show        (GtkWindow *parent, W42View *view);

/* What w42_choice_show calls when the box is answered: the index of
 * the button pressed, or the cancel button if the box was dismissed. */
typedef void (*W42ChoiceFunc) (int choice, gpointer data);

void w42_choice_show (GtkWindow *parent, const char *heading,
                      const char *detail, const char * const *labels,
                      int default_button, int cancel_button,
                      W42ChoiceFunc func, gpointer data);

/* A message with one OK button, in the program's own chrome. */
void w42_message_show (GtkWindow *parent, const char *heading,
                       const char *detail);

G_END_DECLS
