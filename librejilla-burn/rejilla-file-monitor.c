/* -*- Mode: C; indent-tabs-mode: t; c-basic-offset: 8; tab-width: 8 -*- */
/*
 * Librejilla-burn
 * Copyright (C) Philippe Rouquier 2005-2009 <bonfire-app@wanadoo.fr>
 *
 * Librejilla-burn is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * The Librejilla-burn authors hereby grant permission for non-GPL compatible
 * GStreamer plugins to be used and distributed together with GStreamer
 * and Librejilla-burn. This permission is above and beyond the permissions granted
 * by the GPL license by which Librejilla-burn is covered. If you modify this code
 * you may extend this exception to your version of the code, but you are not
 * obligated to do so. If you do not wish to do so, delete this exception
 * statement from your version.
 * 
 * Librejilla-burn is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU Library General Public License for more details.
 * 
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to:
 * 	The Free Software Foundation, Inc.,
 * 	51 Franklin Street, Fifth Floor
 * 	Boston, MA  02110-1301, USA.
 */

#ifdef HAVE_CONFIG_H
#  include <config.h>
#endif

#include <string.h>
#include <errno.h>

#include <glib.h>
#include <glib/gi18n-lib.h>

#include <sys/inotify.h>

#include "rejilla-file-monitor.h"
#include "burn-debug.h"

#include "rejilla-file-node.h"

typedef struct _RejillaFileMonitorPrivate RejillaFileMonitorPrivate;
struct _RejillaFileMonitorPrivate
{
	int notify_id;
	GIOChannel *notify;

	/* In this hash are files/directories we watch individually */
	GHashTable *files;

	/* In this hash are directories whose contents are monitored */
	GHashTable *directories;

	/* This is used in the case of a MOVE_FROM event */
	GSList *moved_list;
};

#define REJILLA_FILE_MONITOR_PRIVATE(o)  (G_TYPE_INSTANCE_GET_PRIVATE ((o), REJILLA_TYPE_FILE_MONITOR, RejillaFileMonitorPrivate))

G_DEFINE_TYPE (RejillaFileMonitor, rejilla_file_monitor, G_TYPE_OBJECT);

struct _RejillaInotifyMovedData {
	gchar *name;
	RejillaFileMonitorType type;
	gpointer callback_data;
	guint32 cookie;
	gint id;
};
typedef struct _RejillaInotifyMovedData RejillaInotifyMovedData;

struct _RejillaInotifyFileData {
	gpointer callback_data;
	gchar *name;
};
typedef struct _RejillaInotifyFileData RejillaInotifyFileData;

struct _RejillaFileMonitorCancelForeach {
	gpointer callback_data;
	RejillaMonitorFindFunc func;

	int dev_fd;

	GSList *results;
};
typedef struct _RejillaFileMonitorCancelForeach RejillaFileMonitorCancelForeach;

struct _RejillaFileMonitorSearchResult {
	gpointer key;
	gpointer callback_data;
};
typedef struct _RejillaFileMonitorSearchResult RejillaFileMonitorSearchResult;

static void
rejilla_inotify_file_data_free (RejillaInotifyFileData *data)
{
	g_free (data->name);
	g_free (data);
}

static void
rejilla_file_monitor_moved_to_event (RejillaFileMonitor *self,
				     gpointer callback_data,
				     const gchar *name,
				     guint32 cookie)
{
	RejillaInotifyMovedData *data = NULL;
	RejillaFileMonitorPrivate *priv;
	RejillaFileMonitorClass *klass;
	GSList *iter;

	priv = REJILLA_FILE_MONITOR_PRIVATE (self);
	klass = REJILLA_FILE_MONITOR_GET_CLASS (self);

	REJILLA_BURN_LOG ("File Monitoring (move to for %s)", name);

	if (!cookie) {
		if (klass->file_added)
			klass->file_added (self, callback_data, name);
		return;
	}

	/* look for a matching cookie */
	for (iter = priv->moved_list; iter; iter = iter->next) {
		data = iter->data;
		if (data->cookie == cookie)
			break;
		data = NULL;
	}

	if (!data) {
		/* It was moved from outside the project since there 
		 * was no moved_from event from a watched directory */
		if (klass->file_added)
			klass->file_added (self, callback_data, name);
		return;
	}

	/* we need to see if it's simple renaming or a real move of a file in
	 * another directory. In the case of a move we can't decide where an
	 * old path (and all its children grafts) should go */

	/* If it's a renaming then the callback_data in source and dest
	 * should be the same */
	if (data->callback_data == callback_data
	&&  data->type == REJILLA_FILE_MONITOR_FOLDER) {
		/* Simple renaming */
		if (klass->file_renamed)
			klass->file_renamed (self,
					     data->type,
					     data->callback_data,
					     data->name,
					     name);
	}
	else {
		/* Move from one watched directory to another watched
		 * directory.
		 * NOTE: there could be renaming at the same time. */
		if (klass->file_moved)
			klass->file_moved (self,
					   data->type,
					   data->callback_data,
					   data->name,
					   callback_data,
					   name);
	}

	/* remove the event from the queue */
	priv->moved_list = g_slist_remove (priv->moved_list, data);
	g_source_remove (data->id);
	g_free (data->name);
	g_free (data);
}

static gboolean
rejilla_file_monitor_move_timeout_cb (RejillaFileMonitor *self)
{
	RejillaInotifyMovedData *data;
	RejillaFileMonitorPrivate *priv;
	RejillaFileMonitorClass *klass;

	priv = REJILLA_FILE_MONITOR_PRIVATE (self);
	klass = REJILLA_FILE_MONITOR_GET_CLASS (self);

	/* an IN_MOVED_FROM timed out. It is the first in the queue. */
	data = priv->moved_list->data;
	priv->moved_list = g_slist_remove (priv->moved_list, data);

	REJILLA_BURN_LOG ("File Monitoring (move timeout for %s)", data->name);

	if (klass->file_removed)
		klass->file_removed (self,
				     data->type,
				     data->callback_data,
				     data->name);

	/* clean up */
	g_free (data->name);
	g_free (data);
	return FALSE;
}

static void
rejilla_file_monitor_moved_from_event (RejillaFileMonitor *self,
				       RejillaFileMonitorType type,
				       gpointer callback_data,
				       const gchar *name,
				       guint32 cookie)
{
	RejillaInotifyMovedData *data = NULL;
	RejillaFileMonitorPrivate *priv;

	priv = REJILLA_FILE_MONITOR_PRIVATE (self);

	REJILLA_BURN_LOG ("File Monitoring (moved from event for %s)", name);

	if (!cookie) {
		RejillaFileMonitorClass *klass;

		klass = REJILLA_FILE_MONITOR_GET_CLASS (self);
		if (klass->file_removed)
			klass->file_removed (self,
					     type,
					     callback_data,
					     name);
		return;
	}

	data = g_new0 (RejillaInotifyMovedData, 1);
	data->type = type;
	data->cookie = cookie;
	data->name = g_strdup (name);
	data->callback_data = callback_data;
			
	/* we remember this move for 5s. If 5s later we haven't received
	 * a corresponding MOVED_TO then we consider the file was removed. */
	data->id = g_timeout_add_seconds (5,
					  (GSourceFunc) rejilla_file_monitor_move_timeout_cb,
					  self);

	/* NOTE: the order is important, we _must_ append them */
	priv->moved_list = g_slist_append (priv->moved_list, data);
}

static void
rejilla_file_monitor_directory_event (RejillaFileMonitor *self,
				      RejillaFileMonitorType type,
				      gpointer callback_data,
				      const gchar *name,
				      struct inotify_event *event)
{
	RejillaFileMonitorClass *klass;

	klass = REJILLA_FILE_MONITOR_GET_CLASS (self);

	/* NOTE: SELF events are only possible here for dummy directories.
	 * As a general rule we don't take heed of the events happening on
	 * the file being monitored, only those that happen inside a directory.
	 * This is done to avoid treating events twice.
	 * IN_DELETE_SELF or IN_MOVE_SELF are therefore not possible here. */
	if (event->mask & IN_ATTRIB) {
		REJILLA_BURN_LOG ("File Monitoring (attributes changed for %s)", name);
		if (klass->file_modified)
			klass->file_modified (self, callback_data, name);
	}
	else if (event->mask & IN_MODIFY) {
		REJILLA_BURN_LOG ("File Monitoring (modified for %s)", name);
		if (klass->file_modified)
			klass->file_modified (self, callback_data, name);
	}
	else if (event->mask & IN_MOVED_FROM) {
		REJILLA_BURN_LOG ("File Monitoring (moved from for %s)", name);
		rejilla_file_monitor_moved_from_event (self, type, callback_data, name, event->cookie);
	}
	else if (event->mask & IN_MOVED_TO) {
		REJILLA_BURN_LOG ("File Monitoring (moved to for %s)", name);
		rejilla_file_monitor_moved_to_event (self, callback_data, name, event->cookie);
	}
	else if (event->mask & (IN_DELETE|IN_UNMOUNT)) {
		REJILLA_BURN_LOG ("File Monitoring (delete/unmount for %s)", name);
		if (klass->file_removed)
			klass->file_removed (self,
					     type,
					     callback_data,
					     name);
	}
	else if (event->mask & IN_CREATE) {
		REJILLA_BURN_LOG ("File Monitoring (create for %s)", name);
		if (klass->file_added)
			klass->file_added (self, callback_data, name);
	}
}

static void
rejilla_file_monitor_inotify_file_event (RejillaFileMonitor *self,
					 GSList *list,
					 const gchar *name,
					 struct inotify_event *event)
{
	RejillaInotifyFileData *data = NULL;
	RejillaFileMonitorPrivate *priv;
	RejillaFileMonitorClass *klass;
	GSList *iter;

	klass = REJILLA_FILE_MONITOR_GET_CLASS (self);
	priv = REJILLA_FILE_MONITOR_PRIVATE (self);

	/* this is a dummy directory used to watch top files so we check
	 * that the file for which that event happened is indeed in 
	 * our selection whether as a single file or as a top directory.
	 * NOTE: since these dummy directories are the real top directories
	 * that's the only case where we treat SELF events, otherwise
	 * to avoid treating events twice, we only choose to treat events
	 * that happened from the parent directory point of view */
	if (!name) {
		/* the event must have happened on this directory */
		if (event->mask & (IN_DELETE_SELF|IN_MOVE_SELF)) {
			/* The directory was moved or removed : find all its
			 * children that are in the selection and remove them.
			 * Remove the watch as well. */
			for (iter = list; iter; iter = iter->next) {
				data = iter->data;

				if (klass->file_removed)
					klass->file_removed (self,
							     REJILLA_FILE_MONITOR_FILE,
							     data->callback_data,
							     name);

				g_free (data);
			}

			g_slist_free (list);
			g_hash_table_remove (priv->files, GINT_TO_POINTER (event->wd));
		}
		else if (event->mask & IN_ATTRIB) {
			/* This is just in case this directory
			 * would become unreadable */
			for (iter = list; iter; iter = iter->next) {
				data = iter->data;

				if (klass->file_modified)
					klass->file_modified (self,
							      data->callback_data,
							      NULL);
			}
		}

		return;
	}

	if (event->mask & IN_MOVED_TO) {
		RejillaInotifyMovedData *moved_data = NULL;
		GSList *iter;

		/* The problem here is that we don't know yet which structure
		 * in the list was moved from in the first place. */

		/* look for a matching cookie */
		for (iter = priv->moved_list; iter; iter = iter->next) {
			moved_data = iter->data;
			if (moved_data->cookie == event->cookie)
				break;

			moved_data = NULL;
		}

		if (!moved_data) {
			/* that wasn't one of ours */
			return;
		}

		if (moved_data->type == REJILLA_FILE_MONITOR_FOLDER) {
			RejillaFileMonitorClass *klass;

			klass = REJILLA_FILE_MONITOR_GET_CLASS (self);
			if (klass->file_removed)
				klass->file_removed (self,
						     moved_data->type,
						     moved_data->callback_data,
						     moved_data->name);
		}
		else {
			gboolean found = FALSE;

			for (iter = list; iter; iter = iter->next) {
				RejillaInotifyFileData *tmp;

				tmp = iter->data;
				if (moved_data->callback_data == tmp->callback_data) {
					RejillaFileMonitorClass *klass;

					found = TRUE;

					/* found one */
					klass = REJILLA_FILE_MONITOR_GET_CLASS (self);

					/* Simple renaming */
					if (klass->file_renamed)
						klass->file_renamed (self,
								     REJILLA_FILE_MONITOR_FILE,
								     tmp->callback_data,
								     moved_data->name,
								     name);

					/* update inotify file structure */
					g_free (tmp->name);
					tmp->name = g_strdup (name);
				}
			}

			if (!found) {
				RejillaFileMonitorClass *klass;

				klass = REJILLA_FILE_MONITOR_GET_CLASS (self);
				if (klass->file_removed)
					klass->file_removed (self,
							     moved_data->type,
							     moved_data->callback_data,
							     moved_data->name);
			}
		}

		/* remove the event from the queue */
		priv->moved_list = g_slist_remove (priv->moved_list, moved_data);
		g_source_remove (moved_data->id);
		g_free (moved_data->name);
		g_free (moved_data);
		return;
	}

	/* There is a name find the right data */
	for (iter = list; iter; iter = iter->next) {
		RejillaInotifyFileData *tmp;

		tmp = iter->data;
		if (!strcmp (tmp->name, name)) {
			data = tmp;
			break;
		}
	}

	/* Then the event happened on a file in the top directory we're not
	 * interested in; return. */
	if (!data)
		return;

	if (event->mask & IN_MOVED_FROM) {
		rejilla_file_monitor_moved_from_event (self,
						       REJILLA_FILE_MONITOR_FILE,
						       data->callback_data,
						       name,
						       event->cookie);
		return;
	}

	/* we just check that this is one of our file (one in
	 * the selection). It must be in a hash table then.
	 * There is an exception though for MOVED_TO event.
	 * indeed that can be the future name of file in the
	 * selection so it isn't yet in the hash tables. */
	rejilla_file_monitor_directory_event (self,
					      REJILLA_FILE_MONITOR_FILE,
					      data->callback_data,
					      NULL,
					      event);
}

static gboolean
rejilla_file_monitor_inotify_monitor_cb (GIOChannel *channel,
					 GIOCondition condition,
					 RejillaFileMonitor *self)
{
	RejillaFileMonitorPrivate *priv;
	struct inotify_event event;
	gpointer callback_data;
	GError *err = NULL;
	GIOStatus status;
	gchar *name;
	gsize size;

	priv = REJILLA_FILE_MONITOR_PRIVATE (self);
	while (condition & G_IO_IN) {
		callback_data = NULL;

		status = g_io_channel_read_chars (channel,
						  (char *) &event,
						  sizeof (struct inotify_event),
						  &size, &err);
		if (status == G_IO_STATUS_EOF)
			return TRUE;

		if (event.len) {
			name = g_new (char, event.len + 1);

			name [event.len] = '\0';

			status = g_io_channel_read_chars (channel,
							  name,
							  event.len,
							  &size,
							  &err);
			if (status != G_IO_STATUS_NORMAL) {
				g_warning ("Error reading inotify: %s\n",
					   err ? "Unknown error" : err->message);
				g_error_free (err);
				return TRUE;
			}
		}
		else
			name = NULL;

		/* look for ignored signal usually following deletion */
		if (event.mask & IN_IGNORED) {
			GSList *list;

			list = g_hash_table_lookup (priv->files, GINT_TO_POINTER (event.wd));
			if (list) {
				g_slist_foreach (list, (GFunc) g_free, NULL);
				g_slist_free (list);
				g_hash_table_remove (priv->files, GINT_TO_POINTER (event.wd));
			}

			g_hash_table_remove (priv->directories, GINT_TO_POINTER (event.wd));

			if (name) {
				g_free (name);
				name = NULL;
			}

			condition = g_io_channel_get_buffer_condition (channel);
			continue;
		}

		callback_data = g_hash_table_lookup (priv->files, GINT_TO_POINTER (event.wd));
		if (!callback_data) {
			/* Retry with children */
			callback_data = g_hash_table_lookup (priv->directories, GINT_TO_POINTER (event.wd));
			if (name && callback_data) {
				/* For directories we don't take heed of the SELF events.
				 * All events are treated through the parent directory
				 * events. */
				rejilla_file_monitor_directory_event (self,
								      REJILLA_FILE_MONITOR_FOLDER,
								      callback_data,
								      name,
								      &event);
			}
			else {
				int dev_fd;

				dev_fd = g_io_channel_unix_get_fd (channel);
				inotify_rm_watch (dev_fd, event.wd);
			}
		}
		else {
			GSList *list;

			/* This is an event happening on the top directory there */
			list = callback_data;
			rejilla_file_monitor_inotify_file_event (self,
								 list,
								 name,
								 &event);
		}

		if (name) {
			g_free (name);
			name = NULL;
		}

		condition = g_io_channel_get_buffer_condition (channel);
	}

	return TRUE;
}

static guint32
rejilla_file_monitor_start_monitoring_real (RejillaFileMonitor *self,
					    const gchar *uri)
{
	RejillaFileMonitorPrivate *priv;
	gchar *unescaped_uri;
	gchar *path;
	gint dev_fd;
	uint32_t mask;
	uint32_t wd;

	priv = REJILLA_FILE_MONITOR_PRIVATE (self);

	unescaped_uri = g_uri_unescape_string (uri, NULL);
	path = g_filename_from_uri (unescaped_uri, NULL, NULL);
	g_free (unescaped_uri);

	dev_fd = g_io_channel_unix_get_fd (priv->notify);
	mask = IN_MODIFY |
	       IN_ATTRIB |
	       IN_MOVED_FROM |
	       IN_MOVED_TO |
	       IN_CREATE |
	       IN_DELETE |
	       IN_DELETE_SELF |
	       IN_MOVE_SELF;

	/* NOTE: always return the same wd when we ask for the same file */
	wd = inotify_add_watch (dev_fd, path, mask);
	if (wd == -1) {
		REJILLA_BURN_LOG ("ERROR creating watch for local file %s : %s\n",
				  path,
				  g_strerror (errno));
		g_free (path);
		return 0;
	}

	g_free (path);
	return wd;
}

/**
 * This is used for top grafted directories in the hierarchies or for
 * single grafted files whose parents are not watched and for which we
 * wouldn't be able to tell if they were renamed */
gboolean
rejilla_file_monitor_single_file (RejillaFileMonitor *self,
				  const gchar *uri,
				  gpointer callback_data)
{
	RejillaFileMonitorPrivate *priv;
	RejillaInotifyFileData *data;
	gchar *parent;
	GSList *list;
	GFile *file;
	guint32 wd;

	priv = REJILLA_FILE_MONITOR_PRIVATE (self);

	/* we want local URIs */
	if (!priv->notify || strncmp (uri, "file://", 7))
		return FALSE;

	/* Start with the parent */
	parent = g_path_get_dirname (uri);
	wd = rejilla_file_monitor_start_monitoring_real (self, parent);
	g_free (parent);

	if (!wd)
		return FALSE;

	/* Since we monitor the parent, put that into a special table */
	data = g_new0 (RejillaInotifyFileData, 1);
	data->callback_data = callback_data;
	file = g_file_new_for_uri (uri);
	data->name = g_file_get_basename (file);
	g_object_unref (file);

	/* inotify always return the same wd for the same file */
	list = g_hash_table_lookup (priv->files, GINT_TO_POINTER (wd));
	list = g_slist_prepend (list, data);
	g_hash_table_insert (priv->files,
			     GINT_TO_POINTER (wd),
			     list);

	/* we only monitor directories. Files are watched through their
	 * parent directory. We give them the same handle as their parent
	 * directory to find it more easily and mark it as being watched */
	return TRUE;
}

gboolean
rejilla_file_monitor_directory_contents (RejillaFileMonitor *self,
				  	 const gchar *uri,
				       	 gpointer callback_data)
{
	RejillaFileMonitorPrivate *priv;
	guint32 wd;

	priv = REJILLA_FILE_MONITOR_PRIVATE (self);

	/* we want local URIs */
	if (!priv->notify || strncmp (uri, "file://", 7))
		return FALSE;

	/* we only monitor directories. Files are watched through their
	 * parent directory. We give them the same handle as their parent
	 * directory to find it more easily and mark it as being watched */
	wd = rejilla_file_monitor_start_monitoring_real (self, uri);

	if (!wd)
		return FALSE;

	g_hash_table_insert (priv->directories,
			     GINT_TO_POINTER (wd),
			     callback_data);
	return TRUE;
}

static void
rejilla_file_monitor_foreach_cancel_file_cb (gpointer key,
					     gpointer hash_data,
					     gpointer callback_data)
{
	GSList *iter;
	RejillaFileMonitorCancelForeach *data = callback_data;

	for (iter = hash_data; iter; iter = iter->next) {
		RejillaInotifyFileData *file_data = NULL;

		file_data = iter->data;
		if (data->func (file_data->callback_data, data->callback_data)) {
			RejillaFileMonitorSearchResult *result;

			result = g_new0 (RejillaFileMonitorSearchResult, 1);
			result->key = key;
			result->callback_data = file_data;
			data->results = g_slist_prepend (data->results, result);

			/* NOTE: don't stop here as func (at least for data 
			 * projects returns TRUE when:
			 * - callback_data is the data looked for
			 * - when there it is an ancestor
			 * So it's never finished with just one hit. */
		}
	}
}

static gboolean
rejilla_file_monitor_foreach_cancel_directory_cb (gpointer key,
						  gpointer hash_data,
						  gpointer callback_data)
{
	int wd = GPOINTER_TO_INT (key);
	RejillaFileMonitorCancelForeach *data = callback_data;

	if (!data->func (hash_data, data->callback_data))
		return FALSE;

	/* really close the watch */
	inotify_rm_watch (data->dev_fd, wd);
	return TRUE;
}

void
rejilla_file_monitor_foreach_cancel (RejillaFileMonitor *self,
				     RejillaMonitorFindFunc func,
				     gpointer callback_data)
{
	GSList *iter;
	GSList *next;
	RejillaFileMonitorPrivate *priv;
	RejillaFileMonitorCancelForeach data;

	priv = REJILLA_FILE_MONITOR_PRIVATE (self);

	data.func = func;
	data.results = NULL;
	data.callback_data = callback_data;
	data.dev_fd = g_io_channel_unix_get_fd (priv->notify);

	g_hash_table_foreach (priv->files,
			      rejilla_file_monitor_foreach_cancel_file_cb,
			      &data);

	for (iter = data.results; iter; iter = iter->next) {
		RejillaFileMonitorSearchResult *result;
		GSList *list;

		result = iter->data;
		list = g_hash_table_lookup (priv->files, result->key);
		list = g_slist_remove (list, result->callback_data);

		rejilla_inotify_file_data_free (result->callback_data);

		if (!list) {
			inotify_rm_watch (data.dev_fd, GPOINTER_TO_INT (result->key));
			g_hash_table_remove (priv->files, result->key);
		}
		else
			g_hash_table_insert (priv->files, result->key, list);

		g_free (result);
	}
	g_slist_free (data.results);

	g_hash_table_foreach_remove (priv->directories,
				     rejilla_file_monitor_foreach_cancel_directory_cb,
				     &data);

	/* Finally get rid of moved that data in moved list */
	for (iter = priv->moved_list; iter; iter = next) {
		RejillaInotifyMovedData *data;

		data = iter->data;
		next = iter->next;
		if (func (data->callback_data, callback_data)) {
			priv->moved_list = g_slist_remove (priv->moved_list, data);
			g_source_remove (data->id);
			g_free (data->name);
			g_free (data);
		}
	}
}

static gboolean
rejilla_file_monitor_foreach_file_reset_cb (gpointer key,
					    gpointer hash_data,
					    gpointer callback_data)
{
	int dev_fd = GPOINTER_TO_INT (callback_data);
	int wd = GPOINTER_TO_INT (key);

	/* really close the watch */
	g_slist_foreach (hash_data, (GFunc) rejilla_inotify_file_data_free, NULL);
	g_slist_free (hash_data);
	inotify_rm_watch (dev_fd, wd);
	return TRUE;
}

static gboolean
rejilla_file_monitor_foreach_directory_reset_cb (gpointer key,
						 gpointer data,
						 gpointer callback_data)
{
	int dev_fd = GPOINTER_TO_INT (callback_data);
	int wd = GPOINTER_TO_INT (key);

	/* really close the watch */
	inotify_rm_watch (dev_fd, wd);
	return TRUE;
}

void
rejilla_file_monitor_reset (RejillaFileMonitor *self)
{
	RejillaFileMonitorPrivate *priv;

	priv = REJILLA_FILE_MONITOR_PRIVATE (self);
	g_hash_table_foreach_remove (priv->files,
				     rejilla_file_monitor_foreach_file_reset_cb,
				     GINT_TO_POINTER (g_io_channel_unix_get_fd (priv->notify)));

	g_hash_table_foreach_remove (priv->directories,
				     rejilla_file_monitor_foreach_directory_reset_cb,
				     GINT_TO_POINTER (g_io_channel_unix_get_fd (priv->notify)));
}

static void
rejilla_file_monitor_init (RejillaFileMonitor *object)
{
	RejillaFileMonitorPrivate *priv;
	int fd;

	priv = REJILLA_FILE_MONITOR_PRIVATE (object);

	priv->files = g_hash_table_new (g_direct_hash, g_direct_equal);
	priv->directories = g_hash_table_new (g_direct_hash, g_direct_equal);

	/* start inotify monitoring backend */
	fd = inotify_init ();
	if (fd != -1) {
		priv->notify = g_io_channel_unix_new (fd);
		g_io_channel_set_encoding (priv->notify, NULL, NULL);
		g_io_channel_set_close_on_unref (priv->notify, TRUE);
		priv->notify_id = g_io_add_watch (priv->notify,
						  G_IO_IN | G_IO_HUP | G_IO_PRI,
						  (GIOFunc) rejilla_file_monitor_inotify_monitor_cb,
						  object);
		g_io_channel_unref (priv->notify);
	}
	else
		g_warning ("Failed to open inotify: %s\n", g_strerror (errno));
}

static void
rejilla_file_monitor_finalize (GObject *object)
{
	RejillaFileMonitorPrivate *priv;

	priv = REJILLA_FILE_MONITOR_PRIVATE (object);

	rejilla_file_monitor_reset (REJILLA_FILE_MONITOR (object));

	if (priv->notify_id)
		g_source_remove (priv->notify_id);

	g_hash_table_destroy (priv->files);
	g_hash_table_destroy (priv->directories);

	G_OBJECT_CLASS (rejilla_file_monitor_parent_class)->finalize (object);
}

static void
rejilla_file_monitor_class_init (RejillaFileMonitorClass *klass)
{
	GObjectClass* object_class = G_OBJECT_CLASS (klass);

	g_type_class_add_private (klass, sizeof (RejillaFileMonitorPrivate));

	object_class->finalize = rejilla_file_monitor_finalize;
}
