/* SPDX-License-Identifier: GPL-3.0-or-later */
#pragma once
#include <gtk/gtk.h>

G_BEGIN_DECLS

/* These APIs are provided by our exact-version GTK patch, not system GTK. */
gboolean gnosis_gdk_wayland_surface_get_target(GdkSurface *surface, gint32 primaries[8], guint32 light[2]);
GdkColorState *gnosis_gdk_wayland_surface_get_preferred(GdkSurface *surface, guint32 luminances[5]);
void gnosis_gdk_wayland_surface_set_content_hdr(GdkSurface *surface, gboolean enabled);
gboolean gnosis_gtk_snapshot_push_browser_surface(GtkSnapshot *snapshot, GdkSurface *surface, gpointer *cached);
void gnosis_gtk_browser_surface_release(gpointer *cached);
gboolean gnosis_gtk_browser_surface_safe_rect(GtkSnapshot *snapshot, GtkWidget *widget, graphene_rect_t *rect);
gboolean gnosis_gdk_frame_timings_get_presentation(GdkFrameTimings *timings, guint32 *refresh, guint32 *flags);

G_END_DECLS
