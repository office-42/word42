/* w42-git.h - a document's versions, kept in Git
 *
 * Copyright (C) 2026 Andreas Røsdal
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * Word 97's File > Versions kept the earlier states of a document inside
 * the document itself.  Word42 keeps them in a Git repository: the one
 * the document's folder is already in, or a new one made in that folder.
 * Each version is a commit of the document's file, so the history is the
 * repository's own and git log, a push to a server or any other Git tool
 * reads it as well as Word42 does.
 *
 * Keeping versions is chosen document by document.  The choice is kept in
 * the repository's configuration, under the document's path:
 *
 *     [word42 "chapters/One.doc"]
 *         versions = true
 *
 * so it stays with the folder when the folder is moved, and a repository
 * of other things is not touched until a document in it asks to be.
 *
 * A commit takes the document's file and nothing else: whatever else has
 * been changed or staged in the repository is left as it was.
 *
 * Built without libgit2, every call fails with G_IO_ERROR_NOT_SUPPORTED,
 * and w42_git_available() says so beforehand.
 */

#pragma once

#include <gio/gio.h>

G_BEGIN_DECLS

typedef struct {
  char   *id;          /* the commit: forty hexadecimal digits */
  char   *short_id;    /* its first seven, as git log --oneline shows it */
  gint64  time;        /* when it was saved, in seconds since 1970, UTC */
  int     offset;      /* the saver's time zone, in minutes east of UTC */
  char   *author;
  char   *message;     /* the whole comment, without its last newline */
} W42GitVersion;

void       w42_git_version_free (W42GitVersion *version);

/* Whether this Word42 was built with Git. */
gboolean   w42_git_available    (void);

/* The folder of the repository `file` is in -- the top of its work tree
 * -- or NULL when it is in none. */
char      *w42_git_repository   (GFile *file);

/* Whether a version of `file` is kept each time it is saved. */
gboolean   w42_git_is_tracked   (GFile *file);

/* Starts or stops keeping versions of `file`.  Starting finds the
 * repository its folder is in or makes one there, and keeps the file as
 * it now is on disk as the first version, with `message` as its comment.
 * Stopping keeps the versions already made. */
gboolean   w42_git_set_tracked  (GFile *file, gboolean on, const char *message,
                                 const char *author, GError **error);

/* Keeps the file as it now is on disk as a version, with `message`,
 * whether or not versions are kept of it on every save: File > Versions >
 * Save Now.  A repository is made in the file's folder when it is in
 * none.  `author` is who saved it when the repository's configuration
 * names no one.  *id is set to the new version's short id, or to NULL when the
 * file is the same as its last version and nothing was kept. */
gboolean   w42_git_commit       (GFile *file, const char *message,
                                 const char *author, char **id,
                                 GError **error);

/* The versions of `file`, newest first: the commits that changed it.
 * A GPtrArray of W42GitVersion*, empty when there are none. */
GPtrArray *w42_git_history      (GFile *file, GError **error);

/* What `file` held in version `id`. */
GBytes    *w42_git_read_version (GFile *file, const char *id, GError **error);

G_END_DECLS
