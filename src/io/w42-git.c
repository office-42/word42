/* w42-git.c - a document's versions, kept in Git
 *
 * Copyright (C) 2026 Andreas Røsdal
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * Everything here is libgit2 on the repository's own files: no git
 * program is run, so none need be installed.  The operations are the few
 * a word processor needs and each is quick on a repository of manuscripts,
 * so they run where they are called rather than in the background.
 */

#include "w42-git.h"

#include <stdlib.h>
#include <string.h>
#include <glib/gi18n.h>

#ifdef HAVE_LIBGIT2
#include <git2.h>
#endif

void
w42_git_version_free (W42GitVersion *version)
{
  if (version == NULL)
    return;
  g_free (version->id);
  g_free (version->short_id);
  g_free (version->author);
  g_free (version->message);
  g_free (version);
}

#ifndef HAVE_LIBGIT2

static void
not_built (GError **error)
{
  g_set_error_literal (error, G_IO_ERROR, G_IO_ERROR_NOT_SUPPORTED,
                       _("This Word42 was built without Git (libgit2), so it cannot "
                         "keep versions of a document."));
}

gboolean
w42_git_available (void)
{
  return FALSE;
}

char *
w42_git_repository (GFile *file)
{
  return NULL;
}

gboolean
w42_git_is_tracked (GFile *file)
{
  return FALSE;
}

gboolean
w42_git_set_tracked (GFile *file, gboolean on, const char *message,
                     const char *author, GError **error)
{
  not_built (error);
  return FALSE;
}

gboolean
w42_git_commit (GFile *file, const char *message, const char *author,
                char **id, GError **error)
{
  if (id != NULL)
    *id = NULL;
  not_built (error);
  return FALSE;
}

GPtrArray *
w42_git_history (GFile *file, GError **error)
{
  not_built (error);
  return NULL;
}

GBytes *
w42_git_read_version (GFile *file, const char *id, GError **error)
{
  not_built (error);
  return NULL;
}

#else /* HAVE_LIBGIT2 */

/* A history walked this far back without an end is a repository of
 * something else with the document in it; its oldest versions can wait. */
#define MAX_COMMITS_WALKED 200000
#define MAX_VERSIONS        5000

gboolean
w42_git_available (void)
{
  return TRUE;
}

static void
git_ready (void)
{
  static gsize done;

  if (g_once_init_enter (&done))
    {
      git_libgit2_init ();
      g_once_init_leave (&done, 1);
    }
}

/* libgit2's own account of what went wrong, which names the file or the
 * object; it is in English, as git's messages are. */
static gboolean
git_failed (int code, GError **error)
{
  const git_error *e = git_error_last ();

  g_set_error_literal (error, G_IO_ERROR,
                       code == GIT_ENOTFOUND ? G_IO_ERROR_NOT_FOUND : G_IO_ERROR_FAILED,
                       e != NULL && e->message != NULL && *e->message != '\0'
                       ? e->message : _("Git could not do what was asked."));
  git_error_clear ();
  return FALSE;
}

/* A folder as the file system knows it, links followed, so that a
 * document reached through a link -- or through macOS's /tmp, which is
 * /private/tmp -- is found in the work tree Git names. */
static char *
real_path (const char *path)
{
#ifdef G_OS_WIN32
  return g_canonicalize_filename (path, NULL);
#else
  char *resolved = realpath (path, NULL);
  char *copy = g_strdup (resolved != NULL ? resolved : path);

  free (resolved);
  return copy;
#endif
}

typedef struct {
  git_repository *repo;
  char           *rel;      /* the file's path in the work tree, with '/' */
} Doc;

static void
doc_close (Doc *doc)
{
  if (doc->repo != NULL)
    git_repository_free (doc->repo);
  g_free (doc->rel);
  memset (doc, 0, sizeof *doc);
}

/* The path of `path` from the top of the work tree, or NULL when it is
 * not under it. */
static char *
doc_relative (git_repository *repo, const char *path)
{
  char *dir = g_path_get_dirname (path);
  char *base = g_path_get_basename (path);
  char *root = real_path (git_repository_workdir (repo));
  char *here = real_path (dir);
  GFile *top = g_file_new_for_path (root);
  GFile *folder = g_file_new_for_path (here);
  char *rel = NULL;

  if (g_file_equal (top, folder))
    rel = g_strdup (base);
  else
    {
      char *sub = g_file_get_relative_path (top, folder);

      if (sub != NULL)
        rel = g_build_filename (sub, base, NULL);
      g_free (sub);
    }
#ifdef G_OS_WIN32
  if (rel != NULL)
    g_strdelimit (rel, "\\", '/');
#endif

  g_object_unref (top);
  g_object_unref (folder);
  g_free (root);
  g_free (here);
  g_free (dir);
  g_free (base);
  return rel;
}

/* The repository `file` is in.  Being in none is not an error: *found
 * says which it was. */
static gboolean
doc_open (GFile *file, Doc *doc, gboolean *found, GError **error)
{
  char *path, *dir;
  int rc;

  memset (doc, 0, sizeof *doc);
  *found = FALSE;
  path = file != NULL ? g_file_get_path (file) : NULL;
  if (path == NULL)
    {
      g_set_error_literal (error, G_IO_ERROR, G_IO_ERROR_NOT_SUPPORTED,
                           _("Versions are kept only of a document saved on this computer."));
      return FALSE;
    }

  git_ready ();
  dir = g_path_get_dirname (path);
  rc = git_repository_open_ext (&doc->repo, dir, 0, NULL);
  g_free (dir);
  if (rc == GIT_ENOTFOUND)
    {
      git_error_clear ();
      doc->repo = NULL;
      g_free (path);
      return TRUE;
    }
  if (rc < 0)
    {
      doc->repo = NULL;
      g_free (path);
      return git_failed (rc, error);
    }
  if (git_repository_is_bare (doc->repo))
    {
      g_set_error_literal (error, G_IO_ERROR, G_IO_ERROR_NOT_SUPPORTED,
                           _("The document's folder is in a Git repository that has no "
                             "working folder, so Word42 cannot keep versions in it."));
      doc_close (doc);
      g_free (path);
      return FALSE;
    }

  doc->rel = doc_relative (doc->repo, path);
  g_free (path);
  if (doc->rel == NULL)
    {
      g_set_error_literal (error, G_IO_ERROR, G_IO_ERROR_FAILED,
                           _("Word42 could not find the document inside its Git repository."));
      doc_close (doc);
      return FALSE;
    }
  *found = TRUE;
  return TRUE;
}

/* The repository `file` is in, made in its folder when there is none:
 * the place a person looking for it would look. */
static gboolean
doc_open_or_make (GFile *file, Doc *doc, GError **error)
{
  char *path, *dir;
  git_repository *made = NULL;
  gboolean found;
  int rc;

  if (!doc_open (file, doc, &found, error))
    return FALSE;
  if (found)
    return TRUE;

  path = g_file_get_path (file);
  dir = g_path_get_dirname (path);
  rc = git_repository_init (&made, dir, 0);
  g_free (dir);
  g_free (path);
  if (rc < 0)
    return git_failed (rc, error);
  git_repository_free (made);
  if (!doc_open (file, doc, &found, error))
    return FALSE;
  if (!found)
    {
      g_set_error_literal (error, G_IO_ERROR, G_IO_ERROR_FAILED,
                           _("Word42 made a Git repository but could not open it."));
      return FALSE;
    }
  return TRUE;
}

/* Where the choice to keep versions is kept: a subsection of the
 * repository's configuration named by the document's path, which Git
 * reads case and all. */
static char *
tracked_key (const char *rel)
{
  return g_strdup_printf ("word42.%s.versions", rel);
}

char *
w42_git_repository (GFile *file)
{
  Doc doc;
  gboolean found;
  char *dir = NULL;

  if (doc_open (file, &doc, &found, NULL) && found)
    {
      char *top = g_strdup (git_repository_workdir (doc.repo));
      gsize n = strlen (top);

      /* libgit2 ends a folder with its separator; a folder named to a
       * person does not. */
      while (n > 1 && (top[n - 1] == '/' || top[n - 1] == '\\'))
        top[--n] = '\0';
      dir = g_canonicalize_filename (top, NULL);
      g_free (top);
    }
  doc_close (&doc);
  return dir;
}

gboolean
w42_git_is_tracked (GFile *file)
{
  Doc doc;
  gboolean found, on = FALSE;

  if (doc_open (file, &doc, &found, NULL) && found)
    {
      git_config *config = NULL;
      char *key = tracked_key (doc.rel);
      int value = 0;

      if (git_repository_config_snapshot (&config, doc.repo) == 0 &&
          git_config_get_bool (&value, config, key) == 0)
        on = value != 0;
      git_error_clear ();
      if (config != NULL)
        git_config_free (config);
      g_free (key);
    }
  doc_close (&doc);
  return on;
}

/* Who saved a version, when the repository's configuration -- or the
 * user's -- names no one: the name Word42 writes as the author of
 * comments and revisions, and an address made as git makes one. */
static int
fallback_signature (git_signature **sig, git_repository *repo, const char *author)
{
  git_config *config = NULL;
  const char *email = NULL;
  char *made = NULL;
  GString *name = g_string_new (NULL);
  const char *from = author != NULL && *author != '\0' ? author : g_get_real_name ();
  int rc;

  /* Git keeps its names between angle brackets and will not have them
   * inside one. */
  for (const char *p = from; p != NULL && *p != '\0'; p++)
    if (*p != '<' && *p != '>' && *p != '\n')
      g_string_append_c (name, *p);
  g_strstrip (name->str);
  if (name->str[0] == '\0' || g_str_equal (name->str, "Unknown"))
    g_string_assign (name, g_get_user_name ());

  if (git_repository_config_snapshot (&config, repo) == 0)
    git_config_get_string (&email, config, "user.email");
  git_error_clear ();
  if (email == NULL || *email == '\0')
    email = made = g_strdup_printf ("%s@%s", g_get_user_name (), g_get_host_name ());

  rc = git_signature_now (sig, name->str, email);

  g_free (made);
  g_string_free (name, TRUE);
  if (config != NULL)
    git_config_free (config);
  return rc;
}

/* The blob `rel` names in `commit`, if it names one. */
static gboolean
entry_id (git_commit *commit, const char *rel, git_oid *out)
{
  git_tree *tree = NULL;
  git_tree_entry *entry = NULL;
  gboolean found = FALSE;

  if (git_commit_tree (&tree, commit) < 0)
    {
      git_error_clear ();
      return FALSE;
    }
  if (git_tree_entry_bypath (&entry, tree, rel) == 0)
    {
      if (git_tree_entry_type (entry) == GIT_OBJECT_BLOB)
        {
          git_oid_cpy (out, git_tree_entry_id (entry));
          found = TRUE;
        }
      git_tree_entry_free (entry);
    }
  git_error_clear ();
  git_tree_free (tree);
  return found;
}

static char *
oid_text (const git_oid *oid, gsize digits)
{
  char buf[72];

  git_oid_tostr (buf, MIN (digits + 1, sizeof buf), oid);
  return g_strdup (buf);
}

/* The file as it is on disk, committed on top of HEAD with only its own
 * entry changed.  Building the commit's tree from HEAD's, not from the
 * index, is what leaves anything else the writer has staged out of it. */
static gboolean
doc_commit (Doc *doc, const char *message, const char *author, char **id, GError **error)
{
  git_oid blob_id, tree_id, commit_id;
  git_reference *head = NULL;
  git_commit *parent = NULL;
  git_tree *base = NULL, *tree = NULL;
  git_signature *sig = NULL;
  git_index *index = NULL;
  git_tree_update update;
  GString *text = NULL;
  gboolean ok = FALSE;
  int rc;

  *id = NULL;
  if (git_repository_state (doc->repo) != GIT_REPOSITORY_STATE_NONE)
    {
      g_set_error_literal (error, G_IO_ERROR, G_IO_ERROR_BUSY,
                           _("The Git repository is in the middle of a merge, a rebase or "
                             "the like. Finish that first; then Word42 can keep versions "
                             "in it again."));
      return FALSE;
    }

  /* As git add would store it, line endings and all. */
  rc = git_blob_create_from_workdir (&blob_id, doc->repo, doc->rel);
  if (rc < 0)
    return git_failed (rc, error);

  rc = git_repository_head (&head, doc->repo);
  if (rc == 0)
    {
      rc = git_reference_peel ((git_object **) &parent, head, GIT_OBJECT_COMMIT);
      if (rc == 0)
        rc = git_commit_tree (&base, parent);
    }
  else if (rc == GIT_EUNBORNBRANCH || rc == GIT_ENOTFOUND)
    {
      /* A repository with nothing in it yet: this is its first commit. */
      git_error_clear ();
      rc = 0;
    }
  if (rc < 0)
    {
      git_failed (rc, error);
      goto out;
    }

  /* Saved again without a change since the last version: that version
   * is this one, and a second copy of it would only clutter the list. */
  if (base != NULL)
    {
      git_tree_entry *entry = NULL;

      if (git_tree_entry_bypath (&entry, base, doc->rel) == 0)
        {
          gboolean same = git_oid_equal (git_tree_entry_id (entry), &blob_id);

          git_tree_entry_free (entry);
          if (same)
            {
              ok = TRUE;
              goto out;
            }
        }
      git_error_clear ();
    }

  memset (&update, 0, sizeof update);
  update.action = GIT_TREE_UPDATE_UPSERT;
  git_oid_cpy (&update.id, &blob_id);
  update.filemode = GIT_FILEMODE_BLOB;
  update.path = doc->rel;
  rc = git_tree_create_updated (&tree_id, doc->repo, base, 1, &update);
  if (rc == 0)
    rc = git_tree_lookup (&tree, doc->repo, &tree_id);
  if (rc < 0)
    {
      git_failed (rc, error);
      goto out;
    }

  rc = git_signature_default (&sig, doc->repo);
  if (rc < 0)
    {
      git_error_clear ();
      rc = fallback_signature (&sig, doc->repo, author);
    }
  if (rc < 0)
    {
      git_failed (rc, error);
      goto out;
    }

  text = g_string_new (message != NULL && *message != '\0' ? message : doc->rel);
  if (text->str[text->len - 1] != '\n')
    g_string_append_c (text, '\n');
  rc = git_commit_create_v (&commit_id, doc->repo, "HEAD", sig, sig, NULL, text->str,
                            tree, parent != NULL ? 1 : 0, parent);
  if (rc < 0)
    {
      git_failed (rc, error);
      goto out;
    }

  /* The index is told too, so that git status shows the file as it is
   * rather than as changed back to what was staged before.  Best effort:
   * the version is kept whether or not the index can be written. */
  if (git_repository_index (&index, doc->repo) == 0 &&
      git_index_add_bypath (index, doc->rel) == 0)
    git_index_write (index);
  git_error_clear ();

  *id = oid_text (&commit_id, 7);
  ok = TRUE;

out:
  if (text != NULL)
    g_string_free (text, TRUE);
  if (index != NULL)
    git_index_free (index);
  if (sig != NULL)
    git_signature_free (sig);
  if (tree != NULL)
    git_tree_free (tree);
  if (base != NULL)
    git_tree_free (base);
  if (parent != NULL)
    git_commit_free (parent);
  if (head != NULL)
    git_reference_free (head);
  return ok;
}

gboolean
w42_git_commit (GFile *file, const char *message, const char *author,
                char **id, GError **error)
{
  Doc doc;
  gboolean ok;
  char *made = NULL;

  if (id != NULL)
    *id = NULL;
  if (!doc_open_or_make (file, &doc, error))
    return FALSE;
  ok = doc_commit (&doc, message, author, &made, error);
  doc_close (&doc);
  if (id != NULL)
    *id = made;
  else
    g_free (made);
  return ok;
}

gboolean
w42_git_set_tracked (GFile *file, gboolean on, const char *message,
                     const char *author, GError **error)
{
  Doc doc;
  gboolean found, ok = TRUE;
  git_config *config = NULL;
  char *key;
  int rc;

  if (!on)
    {
      /* Stopping needs a repository only if there is one to tell. */
      if (!doc_open (file, &doc, &found, error))
        return FALSE;
      if (!found)
        return TRUE;
    }
  else if (!doc_open_or_make (file, &doc, error))
    return FALSE;

  key = tracked_key (doc.rel);
  rc = git_repository_config (&config, doc.repo);
  if (rc == 0)
    rc = on ? git_config_set_bool (config, key, 1) : git_config_delete_entry (config, key);
  if (rc == GIT_ENOTFOUND && !on)
    rc = 0;
  if (rc < 0)
    ok = git_failed (rc, error);
  git_error_clear ();
  if (config != NULL)
    git_config_free (config);
  g_free (key);

  if (ok && on && g_file_query_exists (file, NULL))
    {
      char *id = NULL;

      ok = doc_commit (&doc, message, author, &id, error);
      g_free (id);
    }
  doc_close (&doc);
  return ok;
}

static W42GitVersion *
version_of (git_commit *commit)
{
  W42GitVersion *v = g_new0 (W42GitVersion, 1);
  const git_signature *who = git_commit_author (commit);
  const char *message = git_commit_message (commit);
  char *text;

  v->id = oid_text (git_commit_id (commit), 64);
  v->short_id = oid_text (git_commit_id (commit), 7);
  v->time = who != NULL ? (gint64) who->when.time : (gint64) git_commit_time (commit);
  v->offset = who != NULL ? who->when.offset : git_commit_time_offset (commit);
  v->author = g_utf8_make_valid (who != NULL && who->name != NULL ? who->name : "", -1);
  text = g_utf8_make_valid (message != NULL ? message : "", -1);
  v->message = g_strchomp (text);
  return v;
}

GPtrArray *
w42_git_history (GFile *file, GError **error)
{
  GPtrArray *versions;
  git_revwalk *walk = NULL;
  git_oid oid;
  Doc doc;
  gboolean found;
  guint walked = 0;
  int rc;

  if (!doc_open (file, &doc, &found, error))
    return NULL;
  versions = g_ptr_array_new_with_free_func ((GDestroyNotify) w42_git_version_free);
  if (!found)
    return versions;

  rc = git_revwalk_new (&walk, doc.repo);
  if (rc < 0)
    {
      git_failed (rc, error);
      g_ptr_array_unref (versions);
      doc_close (&doc);
      return NULL;
    }
  git_revwalk_sorting (walk, GIT_SORT_TIME);
  /* An empty repository has no HEAD to walk from, and no versions. */
  if (git_revwalk_push_head (walk) < 0)
    git_error_clear ();
  else
    while (walked++ < MAX_COMMITS_WALKED && versions->len < MAX_VERSIONS &&
           git_revwalk_next (&oid, walk) == 0)
      {
        git_commit *commit = NULL, *parent = NULL;
        git_oid here, before;

        if (git_commit_lookup (&commit, doc.repo, &oid) < 0)
          continue;
        /* A version is a commit whose file differs from its first
         * parent's: one that did not touch it is somebody else's. */
        if (entry_id (commit, doc.rel, &here))
          {
            gboolean changed = TRUE;

            if (git_commit_parentcount (commit) > 0 && git_commit_parent (&parent, commit, 0) == 0)
              {
                changed = !entry_id (parent, doc.rel, &before) || !git_oid_equal (&here, &before);
                git_commit_free (parent);
              }
            if (changed)
              g_ptr_array_add (versions, version_of (commit));
          }
        git_commit_free (commit);
      }
  git_error_clear ();
  git_revwalk_free (walk);
  doc_close (&doc);
  return versions;
}

GBytes *
w42_git_read_version (GFile *file, const char *id, GError **error)
{
  Doc doc;
  gboolean found;
  git_object *object = NULL;
  git_commit *commit = NULL;
  git_tree *tree = NULL;
  git_tree_entry *entry = NULL;
  git_blob *blob = NULL;
  GBytes *bytes = NULL;
  int rc;

  g_return_val_if_fail (id != NULL, NULL);

  if (!doc_open (file, &doc, &found, error))
    return NULL;
  if (!found)
    {
      g_set_error_literal (error, G_IO_ERROR, G_IO_ERROR_NOT_FOUND,
                           _("The document's folder is not in a Git repository."));
      return NULL;
    }

  rc = git_revparse_single (&object, doc.repo, id);
  if (rc == 0)
    rc = git_object_peel ((git_object **) &commit, object, GIT_OBJECT_COMMIT);
  if (rc == 0)
    rc = git_commit_tree (&tree, commit);
  if (rc == 0)
    rc = git_tree_entry_bypath (&entry, tree, doc.rel);
  if (rc == 0)
    rc = git_blob_lookup (&blob, doc.repo, git_tree_entry_id (entry));
  if (rc == 0)
    bytes = g_bytes_new (git_blob_rawcontent (blob), (gsize) git_blob_rawsize (blob));
  else
    git_failed (rc, error);

  if (blob != NULL)
    git_blob_free (blob);
  if (entry != NULL)
    git_tree_entry_free (entry);
  if (tree != NULL)
    git_tree_free (tree);
  if (commit != NULL)
    git_commit_free (commit);
  if (object != NULL)
    git_object_free (object);
  doc_close (&doc);
  return bytes;
}

#endif /* HAVE_LIBGIT2 */
