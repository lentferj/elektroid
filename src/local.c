/*
 *   local.c
 *   Copyright (C) 2021 David García Goñi <dagargo@gmail.com>
 *
 *   This file is part of Elektroid.
 *
 *   Elektroid is free software: you can redistribute it and/or modify
 *   it under the terms of the GNU General Public License as published by
 *   the Free Software Foundation, either version 3 of the License, or
 *   (at your option) any later version.
 *
 *   Elektroid is distributed in the hope that it will be useful,
 *   but WITHOUT ANY WARRANTY; without even the implied warranty of
 *   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *   GNU General Public License for more details.
 *
 *   You should have received a copy of the GNU General Public License
 *   along with Elektroid. If not, see <http://www.gnu.org/licenses/>.
 */

#include <dirent.h>
#include <errno.h>
#include <libgen.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <unistd.h>
#include "local.h"

static gint local_mkdir (const gchar *, void *);

static gint local_delete (const gchar *, void *);

static gint local_rename (const gchar *, const gchar *, void *);

static gint local_read_dir (struct item_iterator *, const gchar *, void *);

static gint local_copy_iterator (struct item_iterator *,
				 struct item_iterator *);

const struct fs_operations FS_LOCAL_OPERATIONS = {
  .fs = 0,
  .readdir = local_read_dir,
  .mkdir = local_mkdir,
  .delete = local_delete,
  .rename = local_rename,
  .move = local_rename,
  .copy = NULL,
  .clear = NULL,
  .swap = NULL,
  .download = NULL,
  .upload = NULL,
  .getid = get_item_name,
  .extension = NULL
};

gint
local_mkdir (const gchar * name, void *data)
{
  DIR *dir;
  gint res = 0;
  gchar *dup;
  gchar *parent;

  dup = strdup (name);
  parent = dirname (dup);

  dir = opendir (parent);
  if (dir)
    {
      closedir (dir);
    }
  else
    {
      res = local_mkdir (parent, data);
      if (res)
	{
	  goto cleanup;
	}
    }

  if (mkdir (name, 0755) == 0 || errno == EEXIST)
    {
      res = 0;
    }
  else
    {
      error_print ("Error while creating dir %s\n", name);
      res = -errno;
    }

cleanup:
  g_free (dup);
  return res;
}

static gint
local_delete (const gchar * path, void *data)
{
  DIR *dir;
  gchar *new_path;
  struct dirent *dirent;

  if ((dir = opendir (path)))
    {
      debug_print (1, "Deleting local %s dir...\n", path);

      while ((dirent = readdir (dir)) != NULL)
	{
	  if (strcmp (dirent->d_name, ".") == 0 ||
	      strcmp (dirent->d_name, "..") == 0)
	    {
	      continue;
	    }
	  new_path = chain_path (path, dirent->d_name);
	  local_delete (new_path, data);
	  free (new_path);
	}

      closedir (dir);

      return rmdir (path);
    }
  else
    {
      debug_print (1, "Deleting local %s file...\n", path);
      return unlink (path);
    }
}

static gint
local_rename (const gchar * old, const gchar * new, void *data)
{
  debug_print (1, "Renaming locally from %s to %s...\n", old, new);
  return rename (old, new);
}

static void
local_free_iterator_data (void *iter_data)
{
  struct local_iterator_data *data = iter_data;
  closedir (data->dir);
  g_free (data->path);
  g_free (data);
}

static guint
local_next_dentry (struct item_iterator *iter)
{
  gchar *full_path;
  struct dirent *dirent;
  struct stat st;
  struct local_iterator_data *data = iter->data;
  guint ret = -ENOENT;

  if (iter->item.name != NULL)
    {
      g_free (iter->item.name);
    }

  while ((dirent = readdir (data->dir)) != NULL)
    {
      if (dirent->d_name[0] == '.')
	{
	  continue;
	}

      if (dirent->d_type == DT_DIR || dirent->d_type == DT_REG)
	{
	  iter->item.name = strdup (dirent->d_name);

	  if (dirent->d_type == DT_DIR)
	    {
	      iter->item.type = ELEKTROID_DIR;
	      iter->item.size = 0;
	    }
	  else
	    {
	      iter->item.type = ELEKTROID_FILE;
	      full_path = chain_path (data->path, dirent->d_name);
	      if (stat (full_path, &st) == 0)
		{
		  iter->item.size = st.st_size;
		}
	      else
		{
		  iter->item.size = 0;
		}
	      free (full_path);
	    }
	  ret = 0;
	  break;
	}
    }

  return ret;
}

static gint
local_init_iterator (struct item_iterator *iter, const gchar * path)
{
  DIR *dir;
  struct local_iterator_data *data;

  if (!(dir = opendir (path)))
    {
      return -errno;
    }

  data = malloc (sizeof (struct local_iterator_data));
  if (!data)
    {
      return -errno;
    }

  data->dir = dir;
  data->path = strdup (path);

  iter->data = data;
  iter->next = local_next_dentry;
  iter->free = local_free_iterator_data;
  iter->copy = local_copy_iterator;
  iter->item.name = NULL;

  return 0;
}

static gint
local_read_dir (struct item_iterator *iter, const gchar * path,
		void *userdata)
{
  return local_init_iterator (iter, path);
}

static gint
local_copy_iterator (struct item_iterator *dst, struct item_iterator *src)
{
  struct local_iterator_data *data = src->data;
  return local_init_iterator (dst, data->path);
}
