/* SPDX-License-Identifier: GPL-3.0-or-later; included inside compositor.c.
 * Page-under-page presentation follows WebKitGTK's ViewGestureControllerGtk.
 * The cached images are immutable GPU textures, never another live webpage.
 */
static void cancel_swipe(GnosisCompositorView *self) {
  if (self->swipe_animation) { gtk_widget_remove_tick_callback(GTK_WIDGET(self), self->swipe_animation); self->swipe_animation = 0; }
  g_clear_object(&self->swipe_source); g_clear_object(&self->swipe_destination);
  self->swipe_progress = 0; self->swipe_loading = self->swipe_loaded = FALSE;
  gtk_widget_queue_draw(GTK_WIDGET(self));
}

GdkTexture *gnosis_compositor_view_capture(GnosisCompositorView *self) {
  GtkNative *native = gtk_widget_get_native(GTK_WIDGET(self));
  int width = gtk_widget_get_width(GTK_WIDGET(self)), height = gtk_widget_get_height(GTK_WIDGET(self));
  if (!native || !self->active || !gtk_widget_get_mapped(GTK_WIDGET(self)) || width <= 0 || height <= 0) return NULL;
  GtkSnapshot *snapshot = gtk_snapshot_new();
  double scale = fmin(1., 1600. / MAX(width, height));
  gtk_snapshot_scale(snapshot, scale, scale);
  snapshot_live(GTK_WIDGET(self), snapshot);
  GskRenderNode *node = gtk_snapshot_free_to_node(snapshot);
  if (!node) return NULL;
  graphene_rect_t bounds = GRAPHENE_RECT_INIT(0, 0, ceil(width * scale), ceil(height * scale));
  GdkTexture *texture = gsk_renderer_render_texture(gtk_native_get_renderer(native), node, &bounds);
  gsk_render_node_unref(node);
  return texture;
}

static void swipe_layer(GtkSnapshot *snapshot, GdkTexture *texture, float x, float width, float height) {
  if (!texture) {
    GdkRGBA background = {.red = 1, .green = 1, .blue = 1, .alpha = 1};
    graphene_rect_t bounds = GRAPHENE_RECT_INIT(x, 0, width, height);
    gtk_snapshot_append_color(snapshot, &background, &bounds); return;
  }
  float scale = MAX(width / gdk_texture_get_width(texture), height / gdk_texture_get_height(texture));
  graphene_rect_t bounds = GRAPHENE_RECT_INIT(x, 0, gdk_texture_get_width(texture) * scale, gdk_texture_get_height(texture) * scale);
  gtk_snapshot_append_texture(snapshot, texture, &bounds);
}

static void snapshot_swipe(GnosisCompositorView *self, GtkSnapshot *snapshot) {
  float width = gtk_widget_get_width(GTK_WIDGET(self)), height = gtk_widget_get_height(GTK_WIDGET(self));
  float progress = CLAMP(self->swipe_progress, 0., 1.);
  gboolean back = self->swipe_direction < 0;
  float edge = (back ? progress : 1 - progress) * width;
  graphene_rect_t bounds = GRAPHENE_RECT_INIT(0, 0, width, height);
  gtk_snapshot_push_clip(snapshot, &bounds);
  swipe_layer(snapshot, back ? self->swipe_destination : self->swipe_source, 0, width, height);
  GdkRGBA dim = {0, 0, 0, .12 * (back ? 1 - progress : progress)};
  gtk_snapshot_append_color(snapshot, &dim, &bounds);
  graphene_rect_t shadow = GRAPHENE_RECT_INIT(edge - 40, 0, 40, height);
  graphene_point_t start = GRAPHENE_POINT_INIT(edge - 40, 0), end = GRAPHENE_POINT_INIT(edge, 0);
  const GskColorStop stops[] = {{0, {0, 0, 0, 0}}, {1, {0, 0, 0, .16}}};
  gtk_snapshot_append_linear_gradient(snapshot, &shadow, &start, &end, stops, G_N_ELEMENTS(stops));
  swipe_layer(snapshot, back ? self->swipe_source : self->swipe_destination, edge, width, height);
  gtk_snapshot_pop(snapshot);
}

void gnosis_compositor_view_begin_swipe(GnosisCompositorView *self, int direction, GdkTexture *destination) {
  cancel_swipe(self);
  self->swipe_source = gnosis_compositor_view_capture(self);
  if (destination) self->swipe_destination = g_object_ref(destination);
  self->swipe_direction = direction;
}

static void update_swipe(GnosisCompositorView *self, double x) {
  if (!self->swipe_source || self->swipe_animation) return;
  self->swipe_progress = CLAMP((self->edge_swipe.x - x) * self->swipe_direction / self->edge_swipe.width, 0., 1.);
  gtk_widget_queue_draw(GTK_WIDGET(self));
}

static gboolean swipe_animation(GtkWidget *widget, GdkFrameClock *clock, gpointer data) {
  (void)data; GnosisCompositorView *self = GNOSIS_COMPOSITOR_VIEW(widget);
  gint64 now = gdk_frame_clock_get_frame_time(clock);
  if (self->swipe_loading) {
    if ((self->swipe_loaded && gdk_frame_clock_get_frame_counter(clock) >= self->swipe_ready_frame) || now >= self->swipe_deadline) {
      self->swipe_animation = 0; cancel_swipe(self); return G_SOURCE_REMOVE;
    }
    return G_SOURCE_CONTINUE;
  }
  double t = CLAMP((double)(now - self->swipe_started) / self->swipe_duration, 0., 1.);
  double eased = 1 - pow(1 - t, 3);
  self->swipe_progress = self->swipe_start_progress + (self->swipe_end_progress - self->swipe_start_progress) * eased;
  gtk_widget_queue_draw(widget);
  if (t < 1) return G_SOURCE_CONTINUE;
  if (!self->swipe_end_progress) {
    self->swipe_animation = 0; cancel_swipe(self); return G_SOURCE_REMOVE;
  }
  self->swipe_loading = TRUE; self->swipe_deadline = now + 5 * G_USEC_PER_SEC;
  g_signal_emit_by_name(self, "touch-navigation", self->swipe_direction);
  return G_SOURCE_CONTINUE;
}

static void end_swipe(GnosisCompositorView *self, int direction) {
  if (!self->swipe_source) { if (direction) g_signal_emit_by_name(self, "touch-navigation", direction); return; }
  if (self->swipe_animation) gtk_widget_remove_tick_callback(GTK_WIDGET(self), self->swipe_animation);
  self->swipe_start_progress = self->swipe_progress; self->swipe_end_progress = direction ? 1 : 0;
  self->swipe_started = gdk_frame_clock_get_frame_time(gtk_widget_get_frame_clock(GTK_WIDGET(self)));
  self->swipe_duration = (150 + 150 * fabs(self->swipe_end_progress - self->swipe_progress)) * 1000;
  gboolean animations = TRUE;
  g_object_get(gtk_widget_get_settings(GTK_WIDGET(self)), "gtk-enable-animations", &animations, NULL);
  if (!animations) self->swipe_duration = 1;
  self->swipe_animation = gtk_widget_add_tick_callback(GTK_WIDGET(self), swipe_animation, NULL, NULL);
}

void gnosis_compositor_view_swipe_ready(GnosisCompositorView *self) {
  self->swipe_loaded = TRUE;
  /* Let the native renderer submit the post-load frame before uncovering it. */
  GdkFrameClock *clock = gtk_widget_get_frame_clock(GTK_WIDGET(self));
  self->swipe_ready_frame = clock ? gdk_frame_clock_get_frame_counter(clock) + 2 : 0;
}
