/*
 *   notifier.h
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

#include <sys/inotify.h>
#include <gtk/gtk.h>
#include "browser.h"

struct notifier
{
  gint fd;
  gint wd;
  size_t event_size;
  struct inotify_event *event;
  gint running;
  struct browser *browser;
};

void notifier_init (struct notifier *, struct browser *);

void notifier_set_dir (struct notifier *, gchar *);

void notifier_close (struct notifier *);

void notifier_free (struct notifier *);

gpointer notifier_run (gpointer);
