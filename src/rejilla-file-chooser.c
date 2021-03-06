/***************************************************************************
 *            rejilla-file-chooser.c
 *
 *  lun mai 29 08:53:18 2006
 *  Copyright  2006  Rouquier Philippe
 *  rejilla-app@wanadoo.fr
 ***************************************************************************/

/*
 *  Rejilla is free software; you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation; either version 2 of the License, or
 *  (at your option) any later version.
 *
 *  Rejilla is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *  GNU Library General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License
 *  along with this program; if not, write to:
 * 	The Free Software Foundation, Inc.,
 * 	51 Franklin Street, Fifth Floor
 * 	Boston, MA  02110-1301, USA.
 */

#ifdef HAVE_CONFIG_H
#  include <config.h>
#endif

#include <string.h>

#include <glib.h>
#include <glib/gi18n-lib.h>
#include <glib-object.h>

#include <gtk/gtk.h>

#include "eggtreemultidnd.h"
#include "rejilla-multi-dnd.h"

#include "rejilla-setting.h"
#include "rejilla-file-chooser.h"
#include "rejilla-uri-container.h"
#include "rejilla-layout-object.h"

static void rejilla_file_chooser_class_init (RejillaFileChooserClass *klass);
static void rejilla_file_chooser_init (RejillaFileChooser *sp);
static void rejilla_file_chooser_iface_uri_container_init (RejillaURIContainerIFace *iface);
static void rejilla_file_chooser_iface_layout_object_init (RejillaLayoutObjectIFace *iface);
static void rejilla_file_chooser_finalize (GObject *object);

static void
rejilla_file_chooser_uri_activated_cb (GtkFileChooser *widget,
				       RejillaFileChooser *chooser);
static void
rejilla_file_chooser_uri_selection_changed_cb (GtkFileChooser *widget,
					       RejillaFileChooser *chooser);
struct RejillaFileChooserPrivate {
	GtkWidget *chooser;

	GtkFileFilter *filter_any;
	GtkFileFilter *filter_audio;
	GtkFileFilter *filter_video;

	RejillaLayoutType type;
};

static GObjectClass *parent_class = NULL;

GType
rejilla_file_chooser_get_type ()
{
	static GType type = 0;

	if(type == 0) {
		static const GTypeInfo our_info = {
			sizeof (RejillaFileChooserClass),
			NULL,
			NULL,
			(GClassInitFunc)rejilla_file_chooser_class_init,
			NULL,
			NULL,
			sizeof (RejillaFileChooser),
			0,
			(GInstanceInitFunc)rejilla_file_chooser_init,
		};

		static const GInterfaceInfo uri_container_info =
		{
			(GInterfaceInitFunc) rejilla_file_chooser_iface_uri_container_init,
			NULL,
			NULL
		};

		static const GInterfaceInfo layout_object =
		{
			(GInterfaceInitFunc) rejilla_file_chooser_iface_layout_object_init,
			NULL,
			NULL
		};

		type = g_type_register_static (GTK_TYPE_ALIGNMENT, 
					       "RejillaFileChooser",
					       &our_info,
					       0);

		g_type_add_interface_static (type,
					     REJILLA_TYPE_URI_CONTAINER,
					     &uri_container_info);
		g_type_add_interface_static (type,
					     REJILLA_TYPE_LAYOUT_OBJECT,
					     &layout_object);
	}

	return type;
}

static void
rejilla_file_chooser_class_init (RejillaFileChooserClass *klass)
{
	GObjectClass *object_class = G_OBJECT_CLASS(klass);

	parent_class = g_type_class_peek_parent(klass);
	object_class->finalize = rejilla_file_chooser_finalize;
}

static void
rejilla_file_chooser_position_percent (GObject *object,
                                       gint width,
                                       gint position)
{
	gint percent;

	percent = position * 10000;
	if (percent % width) {
		percent /= width;
		percent ++;
	}
	else
		percent /= width;

	if (GPOINTER_TO_INT (g_object_get_data (object, "is-stock-file-chooser")))
		rejilla_setting_set_value (rejilla_setting_get_default (),
		                           REJILLA_SETTING_STOCK_FILE_CHOOSER_PERCENT,
		                           GINT_TO_POINTER (percent));
	else
		rejilla_setting_set_value (rejilla_setting_get_default (),
		                           REJILLA_SETTING_REJILLA_FILE_CHOOSER_PERCENT,
		                           GINT_TO_POINTER (percent));
}

static void
rejilla_file_chooser_position_changed (GObject *object,
                                       GParamSpec *param_spec,
                                       gpointer NULL_data)
{
	GtkAllocation allocation = {0, 0};
	gint position;

	gtk_widget_get_allocation (GTK_WIDGET (object), &allocation);
	position = gtk_paned_get_position (GTK_PANED (object));
	rejilla_file_chooser_position_percent (object, allocation.width, position);
}

static void
rejilla_file_chooser_allocation_changed (GtkWidget *widget,
                                         GtkAllocation *allocation,
                                         gpointer NULL_data)
{
	gint position;
	gint width;

	/* See if it's the first allocation. If so set the position and don't
	 * save it */
	if (GPOINTER_TO_INT (g_object_get_data (G_OBJECT (widget), "position_set")) == FALSE) {
		gpointer percent;
		GtkWidget *toplevel;

		toplevel = gtk_widget_get_toplevel (GTK_WIDGET (widget));
		if (G_TYPE_FROM_INSTANCE (toplevel) == GTK_TYPE_FILE_CHOOSER_DIALOG) {
			g_object_set_data (G_OBJECT (widget), "is-stock-file-chooser", GINT_TO_POINTER (1));
			rejilla_setting_get_value (rejilla_setting_get_default (),
				                   REJILLA_SETTING_STOCK_FILE_CHOOSER_PERCENT,
				                   &percent);
		}
		else
			rejilla_setting_get_value (rejilla_setting_get_default (),
				                   REJILLA_SETTING_REJILLA_FILE_CHOOSER_PERCENT,
				                   &percent);

		if (GPOINTER_TO_INT (percent) >= 0) {
			position = allocation->width * GPOINTER_TO_INT (percent) / 10000;
			gtk_paned_set_position (GTK_PANED (widget), position);
		}
		else
			gtk_paned_set_position (GTK_PANED (widget), 30 * allocation->width / 100);

		/* Don't connect to position signal until it was first allocated */
		g_object_set_data (G_OBJECT (widget), "position_set", GINT_TO_POINTER (TRUE));
		g_signal_connect (widget,
		                  "notify::position",
		                  G_CALLBACK (rejilla_file_chooser_position_changed),
		                  NULL);
		return;
	}

	position = gtk_paned_get_position (GTK_PANED (widget));
	width = allocation->width;

	rejilla_file_chooser_position_percent (G_OBJECT (widget), width, position);
}

static void
rejilla_file_chooser_notify_model (GtkTreeView *treeview,
                                   GParamSpec *pspec,
                                   gpointer NULL_data)
{
	GtkTreeModel *model;

	model = gtk_tree_view_get_model (treeview);
	if (model && !EGG_IS_TREE_MULTI_DRAG_SOURCE (model)) {
		GType type;

		type = G_OBJECT_TYPE (model);
		rejilla_enable_multi_DND_for_model_type (type);
	}
}

void
rejilla_file_chooser_customize (GtkWidget *widget, gpointer null_data)
{
	/* we explore everything until we reach a treeview (there are two) */
	if (GTK_IS_TREE_VIEW (widget)) {
		GtkTargetList *list;
		GdkAtom target;
		gboolean found;
		guint num;

		list = gtk_drag_source_get_target_list (widget);
		target = gdk_atom_intern ("text/uri-list", TRUE);
		found = gtk_target_list_find (list, target, &num);
		/* FIXME: should we unref them ? apparently not according to 
		 * the warning messages we get if we do */

		if (found
		&&  gtk_tree_selection_get_mode (gtk_tree_view_get_selection (GTK_TREE_VIEW (widget))) == GTK_SELECTION_MULTIPLE) {
			GtkTreeModel *model;

			/* This is done because GtkFileChooser does not use a
			 * GtkListStore or GtkTreeStore any more. */
			egg_tree_multi_drag_add_drag_support (GTK_TREE_VIEW (widget));
			model = gtk_tree_view_get_model (GTK_TREE_VIEW (widget));
			if (model) {
				GType type;

				type = G_OBJECT_TYPE (model);
				rejilla_enable_multi_DND_for_model_type (type);
			}
			else
				g_signal_connect (widget,
				                  "notify::model",
				                  G_CALLBACK (rejilla_file_chooser_notify_model),
				                  NULL);
		}
	}
	else if (GTK_IS_BUTTON (widget)) {
		GtkWidget *image;
		gchar *stock_id = NULL;

		image = gtk_button_get_image (GTK_BUTTON (widget));
		if (!GTK_IS_IMAGE (image))
			return;

		gtk_image_get_stock (GTK_IMAGE (image), &stock_id, NULL);
		if (stock_id
		&& (!strcmp (stock_id,GTK_STOCK_ADD)
		||  !strcmp (stock_id, GTK_STOCK_REMOVE))) {
			GtkRequisition request;
			gint width;
			GtkWidget *parent;

			/* This is to avoid having the left part too small */
			parent = gtk_widget_get_parent (widget);
			gtk_widget_get_requisition (parent, &request);
			width = request.width;
			gtk_widget_size_request (parent, &request);
			if (request.width >= width)
				gtk_widget_set_size_request (parent,
							     request.width,
							     request.height);
			
			gtk_widget_hide (widget);
		}
	}
	else if (GTK_IS_CONTAINER (widget)) {
		if (GTK_IS_PANED (widget)) {
			GtkWidget *left;

			/* This is to allow the left part to be shrunk as much 
			 * as the user want. */
			left = gtk_paned_get_child1 (GTK_PANED (widget));

			g_object_ref (left);
			gtk_container_remove (GTK_CONTAINER (widget), left);
			gtk_paned_pack1 (GTK_PANED (widget),
					 left,
					 TRUE,
					 TRUE);
			g_object_unref (left);

			g_signal_connect (widget,
			                  "size-allocate",
			                  G_CALLBACK (rejilla_file_chooser_allocation_changed),
			                  NULL);
		}

		gtk_container_foreach (GTK_CONTAINER (widget),
				       rejilla_file_chooser_customize,
				       NULL);
	}
}

static void
rejilla_file_chooser_init (RejillaFileChooser *obj)
{
	GtkFileFilter *filter;

	obj->priv = g_new0 (RejillaFileChooserPrivate, 1);

	obj->priv->chooser = gtk_file_chooser_widget_new (GTK_FILE_CHOOSER_ACTION_OPEN);
	gtk_file_chooser_set_local_only (GTK_FILE_CHOOSER (obj->priv->chooser), FALSE);

	gtk_widget_show (obj->priv->chooser);

	gtk_container_add (GTK_CONTAINER (obj), obj->priv->chooser);

	g_signal_connect (obj->priv->chooser,
			  "file-activated",
			  G_CALLBACK (rejilla_file_chooser_uri_activated_cb),
			  obj);
	g_signal_connect (obj->priv->chooser,
			  "selection-changed",
			  G_CALLBACK (rejilla_file_chooser_uri_selection_changed_cb),
			  obj);

	gtk_file_chooser_set_select_multiple (GTK_FILE_CHOOSER (obj->priv->chooser), TRUE);

	filter = gtk_file_filter_new ();
	gtk_file_filter_set_name (filter, _("All files"));
	gtk_file_filter_add_pattern (filter, "*");
	gtk_file_chooser_add_filter (GTK_FILE_CHOOSER (obj->priv->chooser), filter);

	obj->priv->filter_any = filter;

	filter = gtk_file_filter_new ();
	gtk_file_filter_set_name (filter, _("Audio files"));
	gtk_file_filter_add_mime_type (filter, "audio/*");
	gtk_file_filter_add_mime_type (filter, "application/ogg");
	gtk_file_chooser_add_filter (GTK_FILE_CHOOSER (obj->priv->chooser), filter);

	obj->priv->filter_audio = filter;

	filter = gtk_file_filter_new ();
	gtk_file_filter_set_name (filter, _("Movies"));
	gtk_file_filter_add_mime_type (filter, "video/*");
	gtk_file_filter_add_mime_type (filter, "application/ogg");
	gtk_file_filter_add_mime_type (filter, "application/x-flash-video");
	gtk_file_chooser_add_filter (GTK_FILE_CHOOSER (obj->priv->chooser), filter);

	obj->priv->filter_video = filter;

	filter = gtk_file_filter_new ();
	/* Translators: this is an image, a picture, not a "Disc Image" */
	gtk_file_filter_set_name (filter, C_("picture", "Image files"));
	gtk_file_filter_add_mime_type (filter, "image/*");
	gtk_file_chooser_add_filter (GTK_FILE_CHOOSER (obj->priv->chooser), filter);

	/* this is a hack/workaround to add support for multi DND */
	gtk_container_foreach (GTK_CONTAINER (obj->priv->chooser),
			       rejilla_file_chooser_customize,
			       NULL);
}

static void
rejilla_file_chooser_finalize (GObject *object)
{
	RejillaFileChooser *cobj;

	cobj = REJILLA_FILE_CHOOSER (object);
	g_free (cobj->priv);

	G_OBJECT_CLASS(parent_class)->finalize(object);
}

GtkWidget *
rejilla_file_chooser_new ()
{
	gpointer *obj;
	
	obj = g_object_new (REJILLA_TYPE_FILE_CHOOSER, NULL);
	
	return GTK_WIDGET (obj);
}

static void
rejilla_file_chooser_find_pane (GtkWidget *child,
				gpointer footer)
{
	if (GTK_IS_PANED (child)) {
		GList *children_vbox;
		GList *iter_vbox;
		GtkWidget *vbox;

		vbox = gtk_paned_get_child2 (GTK_PANED (child));
		children_vbox = gtk_container_get_children (GTK_CONTAINER (vbox));
		for (iter_vbox = children_vbox; iter_vbox; iter_vbox = iter_vbox->next) {
			if (GTK_IS_HBOX (iter_vbox->data)) {
				GtkPackType packing;

				gtk_box_query_child_packing (GTK_BOX (vbox),
							     GTK_WIDGET (iter_vbox->data),
							     NULL,
							     NULL,
							     NULL,
							     &packing);

				if (packing == GTK_PACK_START) {
					GtkRequisition total_request, footer_request;

					gtk_widget_size_request (GTK_WIDGET (vbox),
								 &total_request);
					gtk_widget_size_request (GTK_WIDGET (iter_vbox->data),
								 &footer_request);
					*((gint *) footer) = total_request.height - footer_request.height;
					break;
				}
			}
		}
		g_list_free (children_vbox);
	}
	else if (GTK_IS_CONTAINER (child)) {
		gtk_container_foreach (GTK_CONTAINER (child),
				       rejilla_file_chooser_find_pane,
				       footer);
	}
}

static void
rejilla_file_chooser_uri_activated_cb (GtkFileChooser *widget,
				       RejillaFileChooser *chooser)
{
	rejilla_uri_container_uri_activated (REJILLA_URI_CONTAINER (chooser));
}

static void
rejilla_file_chooser_uri_selection_changed_cb (GtkFileChooser *widget,
					       RejillaFileChooser *chooser)
{
	rejilla_uri_container_uri_selected (REJILLA_URI_CONTAINER (chooser));
}

static gchar *
rejilla_file_chooser_get_selected_uri (RejillaURIContainer *container)
{
	RejillaFileChooser *chooser;

	chooser = REJILLA_FILE_CHOOSER (container);
	return gtk_file_chooser_get_uri (GTK_FILE_CHOOSER (chooser->priv->chooser));
}

static gchar **
rejilla_file_chooser_get_selected_uris (RejillaURIContainer *container)
{
	RejillaFileChooser *chooser;
	GSList *list, *iter;
	gchar **uris;
	gint i;

	chooser = REJILLA_FILE_CHOOSER (container);
	list = gtk_file_chooser_get_uris (GTK_FILE_CHOOSER (chooser->priv->chooser));

	uris = g_new0 (gchar*, g_slist_length (list) + 1);
	i = 0;

	for (iter = list; iter; iter = iter->next) {
		uris [i] = iter->data;
		i++;
	}

	g_slist_free (list);

	return uris;
}

static void
rejilla_file_chooser_iface_uri_container_init (RejillaURIContainerIFace *iface)
{
	iface->get_selected_uri = rejilla_file_chooser_get_selected_uri;
	iface->get_selected_uris = rejilla_file_chooser_get_selected_uris;
}

static void
rejilla_file_chooser_set_context (RejillaLayoutObject *object,
				  RejillaLayoutType type)
{
	RejillaFileChooser *self;

	self = REJILLA_FILE_CHOOSER (object);
	if (type == self->priv->type)
		return;

	if (type == REJILLA_LAYOUT_AUDIO)
		gtk_file_chooser_set_filter (GTK_FILE_CHOOSER (self->priv->chooser),
					     self->priv->filter_audio);
	else if (type == REJILLA_LAYOUT_VIDEO)
		gtk_file_chooser_set_filter (GTK_FILE_CHOOSER (self->priv->chooser),
					     self->priv->filter_video);
	else
		gtk_file_chooser_set_filter (GTK_FILE_CHOOSER (self->priv->chooser),
					     self->priv->filter_any);

	self->priv->type = type;
}

static void
rejilla_file_chooser_get_proportion (RejillaLayoutObject *object,
				     gint *header,
				     gint *center,
				     gint *footer)
{
	gtk_container_foreach (GTK_CONTAINER (object),
			       rejilla_file_chooser_find_pane,
			       footer);
}

static void
rejilla_file_chooser_iface_layout_object_init (RejillaLayoutObjectIFace *iface)
{
	iface->get_proportion = rejilla_file_chooser_get_proportion;
	iface->set_context = rejilla_file_chooser_set_context;
}
