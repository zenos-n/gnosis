/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "display-state.h"

/* Reproduce fractional video crops without network, a GPU decoder or YouTube. */
typedef struct { GtkWidget parent; GdkTexture *texture; gpointer offload; } Crop;
typedef GtkWidgetClass CropClass;
G_DEFINE_TYPE(Crop, crop, GTK_TYPE_WIDGET)
static void crop_snapshot(GtkWidget *widget, GtkSnapshot *snapshot) {
  Crop *self = (Crop *)widget;
  GdkSurface *surface = gtk_native_get_surface(gtk_widget_get_native(widget));
  if (!gnosis_gtk_snapshot_push_browser_surface(snapshot, surface, &self->offload)) g_error("offload unavailable");
  /* 1920 / 455 * 455 rounds to 1920.000122, beyond the texture's right edge. */
  const graphene_rect_t clip = GRAPHENE_RECT_INIT(0, 10, 455, 280);
  const graphene_rect_t bounds = GRAPHENE_RECT_INIT(0, 0, 455, 300);
  gtk_snapshot_push_clip(snapshot, &clip);
  gtk_snapshot_append_texture(snapshot, self->texture, &bounds);
  gtk_snapshot_pop(snapshot);
  gtk_snapshot_pop(snapshot);
}
static void crop_dispose(GObject *object) {
  Crop *self = (Crop *)object;
  gnosis_gtk_browser_surface_release(&self->offload);
  g_clear_object(&self->texture);
  G_OBJECT_CLASS(crop_parent_class)->dispose(object);
}
static void crop_class_init(CropClass *klass) {
  klass->snapshot = crop_snapshot;
  G_OBJECT_CLASS(klass)->dispose = crop_dispose;
}
static void crop_init(Crop *self) {
  g_autofree guchar *pixels = g_malloc0(1920 * 1080 * 4);
  g_autoptr(GBytes) bytes = g_bytes_new(pixels, 1920 * 1080 * 4);
  self->texture = gdk_memory_texture_new(1920, 1080, GDK_MEMORY_R8G8B8A8, bytes, 1920 * 4);
  gtk_widget_set_size_request(GTK_WIDGET(self), 500, 350);
}
static gboolean finish(gpointer data) {
  g_main_loop_quit(data);
  return G_SOURCE_REMOVE;
}
int main(void) {
  gtk_init();
  GtkWidget *window = gtk_window_new();
  gtk_window_set_child(GTK_WINDOW(window), g_object_new(crop_get_type(), NULL));
  gtk_window_present(GTK_WINDOW(window));
  GMainLoop *loop = g_main_loop_new(NULL, FALSE);
  g_timeout_add(600, finish, loop);
  g_main_loop_run(loop);
  gtk_window_destroy(GTK_WINDOW(window));
  g_main_loop_unref(loop);
  g_print("HDR_CROP_PASS rounded 1080p source stayed inside its buffer\n");
  return 0;
}
