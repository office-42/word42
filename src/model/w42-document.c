/* w42-document.c - see w42-document.h
 *
 * Copyright (C) 2026 Andreas Røsdal
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "w42-document.h"

#include "w42-io.h"

struct _W42Document {
  GObject        parent_instance;
  W42PieceTable *pt;
  GFile         *file;
  W42PageSetup   page;
  gboolean       modified;
  guint          untitled_number;
  gsize          saved_undo_pos;    /* the undo state when last clean */
  guint64        saved_serial;
  gboolean       unrecorded;      /* a change the undo history does not hold */
};

G_DEFINE_FINAL_TYPE (W42Document, w42_document, G_TYPE_OBJECT)

enum {
  SIGNAL_CHANGED,
  N_SIGNALS
};

static guint signals[N_SIGNALS];

static void
w42_document_finalize (GObject *object)
{
  W42Document *self = W42_DOCUMENT (object);

  g_clear_object (&self->file);
  g_clear_pointer (&self->pt, w42_pt_free);

  G_OBJECT_CLASS (w42_document_parent_class)->finalize (object);
}

static void
w42_document_class_init (W42DocumentClass *klass)
{
  GObjectClass *object_class = G_OBJECT_CLASS (klass);

  object_class->finalize = w42_document_finalize;

  signals[SIGNAL_CHANGED] =
    g_signal_new ("changed", G_TYPE_FROM_CLASS (klass),
                  G_SIGNAL_RUN_LAST, 0, NULL, NULL, NULL,
                  G_TYPE_NONE, 0);
}

static void
w42_document_init (W42Document *self)
{
  static guint counter = 0;

  self->pt = w42_pt_new ();
  self->untitled_number = ++counter;

  /* US Letter with one-inch margins, as Word 6 shipped. */
  self->page.width         = 12240;
  self->page.height        = 15840;
  self->page.margin_left   = 1440;
  self->page.margin_right  = 1440;
  self->page.margin_top    = 1440;
  self->page.margin_bottom = 1440;
}

W42Document *
w42_document_new (void)
{
  return g_object_new (W42_TYPE_DOCUMENT, NULL);
}

W42PieceTable *
w42_document_pt (W42Document *self)
{
  g_return_val_if_fail (W42_IS_DOCUMENT (self), NULL);
  return self->pt;
}

W42ApTable *
w42_document_ap_table (W42Document *self)
{
  g_return_val_if_fail (W42_IS_DOCUMENT (self), NULL);
  return w42_pt_ap_table (self->pt);
}

const W42PageSetup *
w42_document_page_setup (W42Document *self)
{
  g_return_val_if_fail (W42_IS_DOCUMENT (self), NULL);
  return &self->page;
}

void
w42_document_set_page_setup (W42Document *self, const W42PageSetup *page)
{
  g_return_if_fail (W42_IS_DOCUMENT (self));
  g_return_if_fail (page != NULL);

  self->page = *page;
  self->modified = TRUE;
  w42_document_touch (self);
}

void
w42_document_touch (W42Document *self)
{
  g_return_if_fail (W42_IS_DOCUMENT (self));
  g_signal_emit (self, signals[SIGNAL_CHANGED], 0);
}

gboolean
w42_document_get_modified (W42Document *self)
{
  g_return_val_if_fail (W42_IS_DOCUMENT (self), FALSE);
  return self->modified;
}

void
w42_document_set_modified (W42Document *self, gboolean modified)
{
  g_return_if_fail (W42_IS_DOCUMENT (self));
  self->modified = modified;
  if (!modified && self->pt != NULL)
    {
      w42_pt_undo_state (self->pt, &self->saved_undo_pos, &self->saved_serial);
      self->unrecorded = FALSE;
    }
}

/* Some changes are not edits to the text and so are not in the undo
 * history: the stylesheet, the header and footer, the summary
 * information, heading numbering.  Undoing back to where the document
 * was saved does not undo those, so once one has happened the document
 * is unsaved until it is saved. */
void
w42_document_mark_unsaved (W42Document *self)
{
  g_return_if_fail (W42_IS_DOCUMENT (self));
  self->modified = TRUE;
  self->unrecorded = TRUE;
}

gboolean
w42_document_at_saved_state (W42Document *self)
{
  gsize undo_pos = 0;
  guint64 serial = 0;

  g_return_val_if_fail (W42_IS_DOCUMENT (self), FALSE);
  if (self->pt == NULL)
    return TRUE;
  if (self->unrecorded)
    return FALSE;
  w42_pt_undo_state (self->pt, &undo_pos, &serial);
  return undo_pos == self->saved_undo_pos && serial == self->saved_serial;
}

GFile *
w42_document_get_file (W42Document *self)
{
  g_return_val_if_fail (W42_IS_DOCUMENT (self), NULL);
  return self->file;
}

void
w42_document_set_file (W42Document *self, GFile *file)
{
  g_return_if_fail (W42_IS_DOCUMENT (self));

  if (g_set_object (&self->file, file))
    w42_document_touch (self);
}

char *
w42_document_get_title (W42Document *self)
{
  g_return_val_if_fail (W42_IS_DOCUMENT (self), NULL);

  if (self->file != NULL)
    return g_file_get_basename (self->file);

  return g_strdup_printf ("Document%u", self->untitled_number);
}

gboolean
w42_document_load (W42Document *self, GFile *file, GError **error)
{
  g_return_val_if_fail (W42_IS_DOCUMENT (self), FALSE);
  g_return_val_if_fail (G_IS_FILE (file), FALSE);

  if (!w42_io_load (self->pt, &self->page, file, error))
    return FALSE;

  g_set_object (&self->file, file);
  w42_document_set_modified (self, FALSE);
  w42_document_touch (self);

  return TRUE;
}

gboolean
w42_document_save (W42Document *self, GFile *file, GError **error)
{
  g_return_val_if_fail (W42_IS_DOCUMENT (self), FALSE);
  g_return_val_if_fail (G_IS_FILE (file), FALSE);

  if (!w42_io_save (self->pt, &self->page, file, error))
    return FALSE;

  g_set_object (&self->file, file);
  w42_document_set_modified (self, FALSE);
  w42_document_touch (self);

  return TRUE;
}
