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

#include <sys/param.h>

#include <glib.h>
#include <glib/gstdio.h>
#include <glib/gi18n-lib.h>

#include "rejilla-track-data.h"
#include "burn-mkisofs-base.h"
#include "burn-debug.h"

typedef struct _RejillaTrackDataPrivate RejillaTrackDataPrivate;
struct _RejillaTrackDataPrivate
{
	RejillaImageFS fs_type;
	GSList *grafts;
	GSList *excluded;

	guint file_num;
	guint64 data_blocks;
};

#define REJILLA_TRACK_DATA_PRIVATE(o)  (G_TYPE_INSTANCE_GET_PRIVATE ((o), REJILLA_TYPE_TRACK_DATA, RejillaTrackDataPrivate))

G_DEFINE_TYPE (RejillaTrackData, rejilla_track_data, REJILLA_TYPE_TRACK);

/**
 * rejilla_graft_point_free:
 * @graft: a #RejillaGraftPt
 *
 * Frees @graft. Do not use @grafts afterwards.
 *
 **/

void
rejilla_graft_point_free (RejillaGraftPt *graft)
{
	if (graft->uri)
		g_free (graft->uri);

	g_free (graft->path);
	g_free (graft);
}

/**
 * rejilla_graft_point_copy:
 * @graft: a #RejillaGraftPt
 *
 * Copies @graft.
 *
 * Return value: a #RejillaGraftPt.
 **/

RejillaGraftPt *
rejilla_graft_point_copy (RejillaGraftPt *graft)
{
	RejillaGraftPt *newgraft;

	g_return_val_if_fail (graft != NULL, NULL);

	newgraft = g_new0 (RejillaGraftPt, 1);
	newgraft->path = g_strdup (graft->path);
	if (graft->uri)
		newgraft->uri = g_strdup (graft->uri);

	return newgraft;
}

static RejillaBurnResult
rejilla_track_data_set_source_real (RejillaTrackData *track,
				    GSList *grafts,
				    GSList *unreadable)
{
	RejillaTrackDataPrivate *priv;

	g_return_val_if_fail (REJILLA_IS_TRACK_DATA (track), REJILLA_BURN_NOT_SUPPORTED);

	priv = REJILLA_TRACK_DATA_PRIVATE (track);

	if (priv->grafts) {
		g_slist_foreach (priv->grafts, (GFunc) rejilla_graft_point_free, NULL);
		g_slist_free (priv->grafts);
	}

	if (priv->excluded) {
		g_slist_foreach (priv->excluded, (GFunc) g_free, NULL);
		g_slist_free (priv->excluded);
	}

	priv->grafts = grafts;
	priv->excluded = unreadable;
	rejilla_track_changed (REJILLA_TRACK (track));

	return REJILLA_BURN_OK;
}

/**
 * rejilla_track_data_set_source:
 * @track: a #RejillaTrackData.
 * @grafts: (element-type RejillaBurn.GraftPt) (in) (transfer full): a #GSList of #RejillaGraftPt.
 * @unreadable: (element-type utf8) (allow-none) (in) (transfer full): a #GSList of URIS as strings or %NULL.
 *
 * Sets the lists of grafts points (@grafts) and excluded
 * URIs (@unreadable) to be used to create an image.
 *
 * Be careful @track takes ownership of @grafts and
 * @unreadable which must not be freed afterwards.
 *
 * Return value: a #RejillaBurnResult.
 * REJILLA_BURN_OK if it was successful,
 * REJILLA_BURN_ERR otherwise.
 **/

RejillaBurnResult
rejilla_track_data_set_source (RejillaTrackData *track,
			       GSList *grafts,
			       GSList *unreadable)
{
	RejillaTrackDataClass *klass;

	g_return_val_if_fail (REJILLA_IS_TRACK_DATA (track), REJILLA_BURN_ERR);

	klass = REJILLA_TRACK_DATA_GET_CLASS (track);
	return klass->set_source (track, grafts, unreadable);
}

static RejillaBurnResult
rejilla_track_data_add_fs_real (RejillaTrackData *track,
				RejillaImageFS fstype)
{
	RejillaTrackDataPrivate *priv;

	priv = REJILLA_TRACK_DATA_PRIVATE (track);
	priv->fs_type |= fstype;
	return REJILLA_BURN_OK;
}

/**
 * rejilla_track_data_add_fs:
 * @track: a #RejillaTrackData
 * @fstype: a #RejillaImageFS
 *
 * Adds one or more parameters determining the file system type
 * and various other options to create an image.
 *
 * Return value: a #RejillaBurnResult.
 * REJILLA_BURN_OK if it was successful,
 * REJILLA_BURN_ERR otherwise.
 **/

RejillaBurnResult
rejilla_track_data_add_fs (RejillaTrackData *track,
			   RejillaImageFS fstype)
{
	RejillaTrackDataClass *klass;
	RejillaImageFS fs_before;
	RejillaBurnResult result;

	g_return_val_if_fail (REJILLA_IS_TRACK_DATA (track), REJILLA_BURN_NOT_SUPPORTED);

	fs_before = rejilla_track_data_get_fs (track);
	klass = REJILLA_TRACK_DATA_GET_CLASS (track);
	if (!klass->add_fs)
		return REJILLA_BURN_NOT_SUPPORTED;

	result = klass->add_fs (track, fstype);
	if (result != REJILLA_BURN_OK)
		return result;

	if (fs_before != rejilla_track_data_get_fs (track))
		rejilla_track_changed (REJILLA_TRACK (track));

	return REJILLA_BURN_OK;
}

static RejillaBurnResult
rejilla_track_data_rm_fs_real (RejillaTrackData *track,
			       RejillaImageFS fstype)
{
	RejillaTrackDataPrivate *priv;

	priv = REJILLA_TRACK_DATA_PRIVATE (track);
	priv->fs_type &= ~(fstype);
	return REJILLA_BURN_OK;
}

/**
 * rejilla_track_data_rm_fs:
 * @track: a #RejillaTrackData
 * @fstype: a #RejillaImageFS
 *
 * Removes one or more parameters determining the file system type
 * and various other options to create an image.
 *
 * Return value: a #RejillaBurnResult.
 * REJILLA_BURN_OK if it was successful,
 * REJILLA_BURN_ERR otherwise.
 **/

RejillaBurnResult
rejilla_track_data_rm_fs (RejillaTrackData *track,
			  RejillaImageFS fstype)
{
	RejillaTrackDataClass *klass;
	RejillaImageFS fs_before;
	RejillaBurnResult result;

	g_return_val_if_fail (REJILLA_IS_TRACK_DATA (track), REJILLA_BURN_NOT_SUPPORTED);

	fs_before = rejilla_track_data_get_fs (track);
	klass = REJILLA_TRACK_DATA_GET_CLASS (track);
	if (!klass->rm_fs);
		return REJILLA_BURN_NOT_SUPPORTED;

	result = klass->rm_fs (track, fstype);
	if (result != REJILLA_BURN_OK)
		return result;

	if (fs_before != rejilla_track_data_get_fs (track))
		rejilla_track_changed (REJILLA_TRACK (track));

	return REJILLA_BURN_OK;
}

/**
 * rejilla_track_data_set_data_blocks:
 * @track: a #RejillaTrackData
 * @blocks: a #goffset
 *
 * Sets the size of the image to be created (in sectors of 2048 bytes).
 *
 * Return value: a #RejillaBurnResult.
 * REJILLA_BURN_OK if it was successful,
 * REJILLA_BURN_ERR otherwise.
 **/

RejillaBurnResult
rejilla_track_data_set_data_blocks (RejillaTrackData *track,
				    goffset blocks)
{
	RejillaTrackDataPrivate *priv;

	g_return_val_if_fail (REJILLA_IS_TRACK_DATA (track), REJILLA_BURN_NOT_SUPPORTED);

	priv = REJILLA_TRACK_DATA_PRIVATE (track);
	priv->data_blocks = blocks;

	return REJILLA_BURN_OK;
}

/**
 * rejilla_track_data_set_file_num:
 * @track: a #RejillaTrackData
 * @number: a #guint64
 *
 * Sets the number of files (not directories) in @track.
 *
 * Return value: a #RejillaBurnResult.
 * REJILLA_BURN_OK if it was successful,
 * REJILLA_BURN_ERR otherwise.
 **/

RejillaBurnResult
rejilla_track_data_set_file_num (RejillaTrackData *track,
				 guint64 number)
{
	RejillaTrackDataPrivate *priv;

	g_return_val_if_fail (REJILLA_IS_TRACK_DATA (track), REJILLA_BURN_NOT_SUPPORTED);

	priv = REJILLA_TRACK_DATA_PRIVATE (track);

	priv->file_num = number;
	return REJILLA_BURN_OK;
}

/**
 * rejilla_track_data_get_fs:
 * @track: a #RejillaTrackData
 *
 * Returns the parameters determining the file system type
 * and various other options to create an image.
 *
 * Return value: a #RejillaImageFS.
 **/

RejillaImageFS
rejilla_track_data_get_fs (RejillaTrackData *track)
{
	RejillaTrackDataClass *klass;

	g_return_val_if_fail (REJILLA_IS_TRACK_DATA (track), REJILLA_IMAGE_FS_NONE);

	klass = REJILLA_TRACK_DATA_GET_CLASS (track);
	return klass->get_fs (track);
}

static RejillaImageFS
rejilla_track_data_get_fs_real (RejillaTrackData *track)
{
	RejillaTrackDataPrivate *priv;

	priv = REJILLA_TRACK_DATA_PRIVATE (track);
	return priv->fs_type;
}

static GHashTable *
rejilla_track_data_mangle_joliet_name (GHashTable *joliet,
				       const gchar *path,
				       gchar *buffer)
{
	gboolean has_slash = FALSE;
	gint dot_pos = -1;
	gint dot_len = -1;
	gchar *name;
	gint width;
	gint start;
	gint num;
	gint end;

	/* NOTE: this wouldn't work on windows (not a big deal) */
	end = strlen (path);
	if (!end) {
		buffer [0] = '\0';
		return joliet;
	}

	memcpy (buffer, path, MIN (end, MAXPATHLEN));
	buffer [MIN (end, MAXPATHLEN)] = '\0';

	/* move back until we find a character different from G_DIR_SEPARATOR */
	end --;
	while (end >= 0 && G_IS_DIR_SEPARATOR (path [end])) {
		end --;
		has_slash = TRUE;
	}

	/* There are only slashes */
	if (end == -1)
		return joliet;

	start = end - 1;
	while (start >= 0 && !G_IS_DIR_SEPARATOR (path [start])) {
		/* Find the extension while at it */
		if (dot_pos <= 0 && path [start] == '.')
			dot_pos = start;

		start --;
	}

	if (end - start <= 64)
		return joliet;

	name = buffer + start + 1;
	if (dot_pos > 0)
		dot_len = end - dot_pos + 1;

	if (dot_len > 1 && dot_len < 5)
		memcpy (name + 64 - dot_len,
			path + dot_pos,
			dot_len);

	name [64] = '\0';

	if (!joliet) {
		joliet = g_hash_table_new_full (g_str_hash,
						g_str_equal,
						g_free,
						NULL);

		g_hash_table_insert (joliet, g_strdup (buffer), GINT_TO_POINTER (1));
		if (has_slash)
			strcat (buffer, G_DIR_SEPARATOR_S);

		REJILLA_BURN_LOG ("Mangled name to %s (truncated)", buffer);
		return joliet;
	}

	/* see if this path was already used */
	num = GPOINTER_TO_INT (g_hash_table_lookup (joliet, buffer));
	if (!num) {
		g_hash_table_insert (joliet, g_strdup (buffer), GINT_TO_POINTER (1));

		if (has_slash)
			strcat (buffer, G_DIR_SEPARATOR_S);

		REJILLA_BURN_LOG ("Mangled name to %s (truncated)", buffer);
		return joliet;
	}

	/* NOTE: g_hash_table_insert frees key_path */
	num ++;
	g_hash_table_insert (joliet, g_strdup (buffer), GINT_TO_POINTER (num));

	width = 1;
	while (num / (width * 10)) width ++;

	/* try to keep the extension */
	if (dot_len < 5 && dot_len > 1 )
		sprintf (name + (64 - width - dot_len),
			 "%i%s",
			 num,
			 path + dot_pos);
	else
		sprintf (name + (64 - width),
			 "%i",
			 num);

	if (has_slash)
		strcat (buffer, G_DIR_SEPARATOR_S);

	REJILLA_BURN_LOG ("Mangled name to %s", buffer);
	return joliet;
}

/**
 * rejilla_track_data_get_grafts:
 * @track: a #RejillaTrackData
 *
 * Returns a list of #RejillaGraftPt.
 *
 * Do not free after usage as @track retains ownership.
 *
 * Return value: (transfer none) (element-type RejillaBurn.GraftPt) (allow-none): a #GSList of #RejillaGraftPt or %NULL if empty.
 **/

GSList *
rejilla_track_data_get_grafts (RejillaTrackData *track)
{
	RejillaTrackDataClass *klass;
	GHashTable *mangle = NULL;
	RejillaImageFS image_fs;
	GSList *grafts;
	GSList *iter;

	g_return_val_if_fail (REJILLA_IS_TRACK_DATA (track), NULL);

	klass = REJILLA_TRACK_DATA_GET_CLASS (track);
	grafts = klass->get_grafts (track);

	image_fs = rejilla_track_data_get_fs (track);
	if ((image_fs & REJILLA_IMAGE_FS_JOLIET) == 0)
		return grafts;

	for (iter = grafts; iter; iter = iter->next) {
		RejillaGraftPt *graft;
		gchar newpath [MAXPATHLEN];

		graft = iter->data;
		mangle = rejilla_track_data_mangle_joliet_name (mangle,
								graft->path,
								newpath);

		g_free (graft->path);
		graft->path = g_strdup (newpath);
	}

	if (mangle)
		g_hash_table_destroy (mangle);

	return grafts;
}

static GSList *
rejilla_track_data_get_grafts_real (RejillaTrackData *track)
{
	RejillaTrackDataPrivate *priv;

	priv = REJILLA_TRACK_DATA_PRIVATE (track);
	return priv->grafts;
}

/**
 * rejilla_track_data_get_excluded_list:
 * @track: a #RejillaTrackData.
 *
 * Returns a list of URIs which must not be included in
 * the image to be created.
 * Do not free the list or any of the URIs after
 * usage as @track retains ownership.
 *
 * Return value: (transfer none) (element-type utf8) (allow-none): a #GSList of #gchar * or %NULL if no
 * URI should be excluded.
 **/

GSList *
rejilla_track_data_get_excluded_list (RejillaTrackData *track)
{
	RejillaTrackDataClass *klass;

	g_return_val_if_fail (REJILLA_IS_TRACK_DATA (track), NULL);

	klass = REJILLA_TRACK_DATA_GET_CLASS (track);
	return klass->get_excluded (track);
}

/**
 * rejilla_track_data_get_excluded:
 * @track: a #RejillaTrackData.
 * @copy: a #gboolean.
 *
 * Returns a list of URIs which must not be included in
 * the image to be created.
 * If @copy is %TRUE then the @list is a copy and must
 * be freed once it is not needed anymore. If %FALSE,
 * do not free after usage as @track retains ownership.
 *
 * Deprecated since 2.29.2
 *
 * Return value: a #GSList of #gchar * or %NULL if no
 * URI should be excluded.
 **/

G_GNUC_DEPRECATED GSList *
rejilla_track_data_get_excluded (RejillaTrackData *track,
				 gboolean copy)
{
	RejillaTrackDataClass *klass;
	GSList *retval = NULL;
	GSList *excluded;
	GSList *iter;

	g_return_val_if_fail (REJILLA_IS_TRACK_DATA (track), NULL);

	klass = REJILLA_TRACK_DATA_GET_CLASS (track);
	excluded = klass->get_excluded (track);
	if (!copy)
		return excluded;

	for (iter = excluded; iter; iter = iter->next) {
		gchar *uri;

		uri = iter->data;
		retval = g_slist_prepend (retval, g_strdup (uri));
	}

	return retval;
}

static GSList *
rejilla_track_data_get_excluded_real (RejillaTrackData *track)
{
	RejillaTrackDataPrivate *priv;

	priv = REJILLA_TRACK_DATA_PRIVATE (track);
	return priv->excluded;
}

/**
 * rejilla_track_data_get_paths:
 * @track: a #RejillaTrackData.
 * @use_joliet: a #gboolean.
 * @grafts_path: a #gchar.
 * @excluded_path: a #gchar.
 * @emptydir: a #gchar.
 * @videodir: (allow-none): a #gchar or %NULL.
 * @error: a #GError.
 *
 * Write in @grafts_path (a path to a file) the graft points,
 * in @excluded_path (a path to a file) the list of paths to
 * be excluded, @emptydir (a path to a file) an empty
 * directory to be used for created directories, @videodir
 * (a path to a file) for a directory to be used to build the
 * the video image.
 *
 * This is mostly for internal use by mkisofs and similar.
 *
 * This function takes care of mangling.
 *
 * Deprecated since 2.29.2
 *
 * Return value: a #RejillaBurnResult.
 **/

G_GNUC_DEPRECATED RejillaBurnResult
rejilla_track_data_get_paths (RejillaTrackData *track,
			      gboolean use_joliet,
			      const gchar *grafts_path,
			      const gchar *excluded_path,
			      const gchar *emptydir,
			      const gchar *videodir,
			      GError **error)
{
	GSList *grafts;
	GSList *excluded;
	RejillaBurnResult result;
	RejillaTrackDataClass *klass;

	g_return_val_if_fail (REJILLA_IS_TRACK_DATA (track), REJILLA_BURN_NOT_SUPPORTED);

	klass = REJILLA_TRACK_DATA_GET_CLASS (track);
	grafts = klass->get_grafts (track);
	excluded = klass->get_excluded (track);

	result = rejilla_mkisofs_base_write_to_files (grafts,
						      excluded,
						      use_joliet,
						      emptydir,
						      videodir,
						      grafts_path,
						      excluded_path,
						      error);
	return result;
}

/**
 * rejilla_track_data_write_to_paths:
 * @track: a #RejillaTrackData.
 * @grafts_path: a #gchar.
 * @excluded_path: a #gchar.
 * @emptydir: a #gchar.
 * @videodir: (allow-none): a #gchar or %NULL.
 * @error: a #GError.
 *
 * Write to @grafts_path (a path to a file) the graft points,
 * and to @excluded_path (a path to a file) the list of paths to
 * be excluded; @emptydir is (path) is an empty
 * directory to be used for created directories;
 * @videodir (a path) is a directory to be used to build the
 * the video image.
 *
 * This is mostly for internal use by mkisofs and similar.
 *
 * This function takes care of file name mangling.
 *
 * Return value: a #RejillaBurnResult.
 **/

RejillaBurnResult
rejilla_track_data_write_to_paths (RejillaTrackData *track,
                                   const gchar *grafts_path,
                                   const gchar *excluded_path,
                                   const gchar *emptydir,
                                   const gchar *videodir,
                                   GError **error)
{
	GSList *grafts;
	GSList *excluded;
	RejillaBurnResult result;
	RejillaTrackDataClass *klass;

	g_return_val_if_fail (REJILLA_IS_TRACK_DATA (track), REJILLA_BURN_NOT_SUPPORTED);

	klass = REJILLA_TRACK_DATA_GET_CLASS (track);
	grafts = klass->get_grafts (track);
	excluded = klass->get_excluded (track);

	result = rejilla_mkisofs_base_write_to_files (grafts,
						      excluded,
						      rejilla_track_data_get_fs (track),
						      emptydir,
						      videodir,
						      grafts_path,
						      excluded_path,
						      error);
	return result;
}

/**
 * rejilla_track_data_get_file_num:
 * @track: a #RejillaTrackData.
 * @file_num: (allow-none) (out): a #guint64 or %NULL.
 *
 * Sets the number of files (not directories) in @file_num.
 *
 * Return value: a #RejillaBurnResult. %TRUE if @file_num
 * was set, %FALSE otherwise.
 **/

RejillaBurnResult
rejilla_track_data_get_file_num (RejillaTrackData *track,
				 guint64 *file_num)
{
	RejillaTrackDataClass *klass;

	g_return_val_if_fail (REJILLA_IS_TRACK_DATA (track), 0);

	klass = REJILLA_TRACK_DATA_GET_CLASS (track);
	if (file_num)
		*file_num = klass->get_file_num (track);

	return REJILLA_BURN_OK;
}

static guint64
rejilla_track_data_get_file_num_real (RejillaTrackData *track)
{
	RejillaTrackDataPrivate *priv;

	priv = REJILLA_TRACK_DATA_PRIVATE (track);
	return priv->file_num;
}

static RejillaBurnResult
rejilla_track_data_get_size (RejillaTrack *track,
			     goffset *blocks,
			     goffset *block_size)
{
	RejillaTrackDataPrivate *priv;

	priv = REJILLA_TRACK_DATA_PRIVATE (track);

	if (*block_size)
		*block_size = 2048;

	if (*blocks)
		*blocks = priv->data_blocks;

	return REJILLA_BURN_OK;
}

static RejillaBurnResult
rejilla_track_data_get_track_type (RejillaTrack *track,
				   RejillaTrackType *type)
{
	RejillaTrackDataPrivate *priv;

	priv = REJILLA_TRACK_DATA_PRIVATE (track);

	rejilla_track_type_set_has_data (type);
	rejilla_track_type_set_data_fs (type, priv->fs_type);

	return REJILLA_BURN_OK;
}

static RejillaBurnResult
rejilla_track_data_get_status (RejillaTrack *track,
			       RejillaStatus *status)
{
	RejillaTrackDataPrivate *priv;

	priv = REJILLA_TRACK_DATA_PRIVATE (track);

	if (!priv->grafts) {
		if (status)
			rejilla_status_set_error (status,
						  g_error_new (REJILLA_BURN_ERROR,
							       REJILLA_BURN_ERROR_EMPTY,
							       _("There are no files to write to disc")));
		return REJILLA_BURN_ERR;
	}

	return REJILLA_BURN_OK;
}

static void
rejilla_track_data_init (RejillaTrackData *object)
{ }

static void
rejilla_track_data_finalize (GObject *object)
{
	RejillaTrackDataPrivate *priv;

	priv = REJILLA_TRACK_DATA_PRIVATE (object);
	if (priv->grafts) {
		g_slist_foreach (priv->grafts, (GFunc) rejilla_graft_point_free, NULL);
		g_slist_free (priv->grafts);
		priv->grafts = NULL;
	}

	if (priv->excluded) {
		g_slist_foreach (priv->excluded, (GFunc) g_free, NULL);
		g_slist_free (priv->excluded);
		priv->excluded = NULL;
	}

	G_OBJECT_CLASS (rejilla_track_data_parent_class)->finalize (object);
}

static void
rejilla_track_data_class_init (RejillaTrackDataClass *klass)
{
	GObjectClass *object_class = G_OBJECT_CLASS (klass);
	RejillaTrackClass *track_class = REJILLA_TRACK_CLASS (klass);
	RejillaTrackDataClass *track_data_class = REJILLA_TRACK_DATA_CLASS (klass);

	g_type_class_add_private (klass, sizeof (RejillaTrackDataPrivate));

	object_class->finalize = rejilla_track_data_finalize;

	track_class->get_type = rejilla_track_data_get_track_type;
	track_class->get_status = rejilla_track_data_get_status;
	track_class->get_size = rejilla_track_data_get_size;

	track_data_class->set_source = rejilla_track_data_set_source_real;
	track_data_class->add_fs = rejilla_track_data_add_fs_real;
	track_data_class->rm_fs = rejilla_track_data_rm_fs_real;

	track_data_class->get_fs = rejilla_track_data_get_fs_real;
	track_data_class->get_grafts = rejilla_track_data_get_grafts_real;
	track_data_class->get_excluded = rejilla_track_data_get_excluded_real;
	track_data_class->get_file_num = rejilla_track_data_get_file_num_real;
}

/**
 * rejilla_track_data_new:
 *
 * Creates a new #RejillaTrackData.
 * 
 *This type of tracks is used to create a disc image
 * from or burn a selection of files.
 *
 * Return value: a #RejillaTrackData
 **/

RejillaTrackData *
rejilla_track_data_new (void)
{
	return g_object_new (REJILLA_TYPE_TRACK_DATA, NULL);
}
