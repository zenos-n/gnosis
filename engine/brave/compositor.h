/* SPDX-License-Identifier: GPL-3.0-or-later */
#pragma once
#include <gtk/gtk.h>
G_BEGIN_DECLS

gboolean gnosis_compositor_is_host_shortcut(guint keyval, GdkModifierType modifiers);
const char *gnosis_compositor_vim_key(guint keyval, GdkModifierType modifiers);
#define GNOSIS_TYPE_COMPOSITOR (gnosis_compositor_get_type())
G_DECLARE_FINAL_TYPE(GnosisCompositor, gnosis_compositor, GNOSIS, COMPOSITOR, GObject)
GnosisCompositor *gnosis_compositor_new(GError **error);
const char *gnosis_compositor_get_socket(GnosisCompositor *self);
#define GNOSIS_TYPE_COMPOSITOR_VIEW (gnosis_compositor_view_get_type())
G_DECLARE_FINAL_TYPE(GnosisCompositorView, gnosis_compositor_view, GNOSIS, COMPOSITOR_VIEW, GtkWidget)
void gnosis_compositor_view_set_vim_field(GnosisCompositorView *self, gboolean enabled);
void gnosis_compositor_view_set_navigation_gestures(GnosisCompositorView *self, gboolean enabled);
GdkTexture *gnosis_compositor_view_capture(GnosisCompositorView *self);
void gnosis_compositor_view_begin_swipe(GnosisCompositorView *self, int direction, GdkTexture *destination);
void gnosis_compositor_view_swipe_ready(GnosisCompositorView *self);
GtkWidget *gnosis_compositor_view_new(GError **error);
GtkWidget *gnosis_compositor_view_new_for_server(GnosisCompositor *server, const char *token);
gboolean gnosis_compositor_view_has_surface(GnosisCompositorView *self);
const char *gnosis_compositor_view_get_socket(GnosisCompositorView *self);
gboolean gnosis_compositor_view_edit(GnosisCompositorView *self, const char *command);
gboolean gnosis_compositor_view_context_position(GnosisCompositorView *self, double *x, double *y);
gboolean gnosis_compositor_view_caret_rect(GnosisCompositorView *self, GdkRectangle *rect);
void gnosis_compositor_view_clear_caret(GnosisCompositorView *self);
void gnosis_compositor_view_hold_caret(GnosisCompositorView *self, gboolean held);
void gnosis_compositor_view_release_buttons(GnosisCompositorView *self);
void gnosis_compositor_drag_set_image(GdkDrag *drag, GdkTexture *image);
G_END_DECLS
