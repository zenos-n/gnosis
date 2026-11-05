/* SPDX-License-Identifier: GPL-3.0-or-later; included inside compositor.c. */
typedef struct {
  int32_t id;
  double offset_x, offset_y;
} TouchPoint;

static gboolean touch_frame(gpointer data) {
  GnosisCompositor *server = data;
  server->touch_frame = 0;
  wlr_seat_touch_notify_frame(server->seat);
  wl_display_flush_clients(server->display);
  return G_SOURCE_REMOVE;
}
static void schedule_touch_frame(GnosisCompositor *server) {
  /* GDK delivers the contacts of one outer touch frame as individual events. */
  if (!server->touch_frame) server->touch_frame = g_idle_add_full(G_PRIORITY_HIGH_IDLE, touch_frame, server, NULL);
}

static void view_coordinates(GnosisCompositorView *self, double *x, double *y) {
  struct wlr_box geometry = self->active->toplevel->base->geometry;
  int width = gtk_widget_get_width(GTK_WIDGET(self)), height = gtk_widget_get_height(GTK_WIDGET(self));
  if (width > 0 && geometry.width > 0) *x *= (double)geometry.width / width;
  if (height > 0 && geometry.height > 0) *y *= (double)geometry.height / height;
  *x += geometry.x; *y += geometry.y;
}

static guint touch_count(GnosisCompositor *server) {
  guint count = 0;
  for (guint i = 0; i < server->hosts->len; i++) {
    GnosisCompositorView *host = g_ptr_array_index(server->hosts, i);
    count += g_hash_table_size(host->touches);
  }
  return count;
}

static void cancel_touches(GnosisCompositorView *self) {
  if (!self->touches || !g_hash_table_size(self->touches)) return;
  if (self->server && self->server->seat) {
    GHashTableIter iter; gpointer value;
    g_hash_table_iter_init(&iter, self->touches);
    while (g_hash_table_iter_next(&iter, NULL, &value)) {
      struct wlr_touch_point *point = wlr_seat_touch_get_point(self->server->seat, ((TouchPoint *)value)->id);
      if (point) wlr_seat_touch_notify_cancel(self->server->seat, point->client);
    }
    /* wl_touch.cancel covers every contact of that client, including other tabs. */
    for (guint i = 0; i < self->server->hosts->len; i++) {
      GnosisCompositorView *host = g_ptr_array_index(self->server->hosts, i);
      host->edge_swipe.eligible = false;
    }
    wlr_seat_touch_notify_frame(self->server->seat);
    wl_display_flush_clients(self->server->display);
  }
  g_hash_table_remove_all(self->touches);
  self->edge_swipe = (GnosisEdgeSwipe){0};
}

static gboolean touch_event(GtkEventControllerLegacy *controller, GdkEvent *event,
                            GnosisCompositorView *self) {
  (void)controller;
  GdkEventType type = gdk_event_get_event_type(event);
  if (type != GDK_TOUCH_BEGIN && type != GDK_TOUCH_UPDATE && type != GDK_TOUCH_END && type != GDK_TOUCH_CANCEL) return FALSE;
  if (!self->server || !self->active) return TRUE;
  if (type == GDK_TOUCH_CANCEL) { cancel_touches(self); return TRUE; }
  GdkEventSequence *sequence = gdk_event_get_event_sequence(event);
  double x, y, tx, ty;
  GtkNative *native = gtk_native_get_for_surface(gdk_event_get_surface(event));
  if (!native || !gdk_event_get_position(event, &x, &y)) return TRUE;
  gtk_native_get_surface_transform(native, &tx, &ty);
  graphene_point_t source = GRAPHENE_POINT_INIT(x + tx, y + ty), local;
  if (!gtk_widget_compute_point(GTK_WIDGET(native), GTK_WIDGET(self), &source, &local) ||
      !isfinite(local.x) || !isfinite(local.y)) return TRUE;
  x = local.x; y = local.y;
  double sx = x, sy = y;
  view_coordinates(self, &sx, &sy);
  struct wlr_seat *seat = self->server->seat;
  guint32 time = gdk_event_get_time(event);
  TouchPoint *touch = g_hash_table_lookup(self->touches, sequence);
  if (type == GDK_TOUCH_BEGIN) {
    if (touch) { cancel_touches(self); return TRUE; }
    double px, py;
    struct wlr_surface *surface = wlr_xdg_surface_surface_at(self->active->toplevel->base, sx, sy, &px, &py);
    if (!surface) return TRUE;
    if (!g_hash_table_size(self->touches))
      gnosis_edge_swipe_begin(&self->edge_swipe, x, y, gtk_widget_get_width(GTK_WIDGET(self)), self->navigation_gestures);
    touch = g_new0(TouchPoint, 1);
    do { touch->id = (self->server->next_touch_id++ & G_MAXINT32); }
    while (wlr_seat_touch_get_point(seat, touch->id));
    touch->offset_x = px - sx; touch->offset_y = py - sy;
    g_hash_table_insert(self->touches, sequence, touch);
    if (touch_count(self->server) > 1) {
      for (guint i = 0; i < self->server->hosts->len; i++) {
        GnosisCompositorView *host = g_ptr_array_index(self->server->hosts, i);
        host->edge_swipe.eligible = false;
      }
    }
    /* Once an edge swipe owns the gesture, extra fingers cancel navigation and
     * stay suppressed until release; they must not restart a page gesture. */
    if (!self->edge_swipe.owned) {
      gtk_widget_grab_focus(GTK_WIDGET(self));
      wlr_seat_touch_notify_down(seat, surface, time, touch->id, px, py);
    }
  } else if (touch) {
    struct wlr_touch_point *point = wlr_seat_touch_get_point(seat, touch->id);
    if (!point || !point->surface) {
      if (!self->edge_swipe.owned) self->edge_swipe.eligible = false;
    }
    if (type == GDK_TOUCH_UPDATE) {
      if (point && self->edge_swipe.eligible && gnosis_edge_swipe_move(&self->edge_swipe, x, y)) {
        wlr_seat_touch_notify_cancel(seat, point->client);
      } else if (point && point->surface) {
        wlr_seat_touch_notify_motion(seat, time, touch->id, sx + touch->offset_x, sy + touch->offset_y);
      }
    } else if (type == GDK_TOUCH_END) {
      int direction = self->navigation_gestures ? gnosis_edge_swipe_finish(&self->edge_swipe, x, y) : 0;
      if (point) wlr_seat_touch_notify_up(seat, time, touch->id);
      g_hash_table_remove(self->touches, sequence);
      if (!g_hash_table_size(self->touches)) self->edge_swipe = (GnosisEdgeSwipe){0};
      wlr_seat_touch_notify_frame(seat);
      wl_display_flush_clients(self->server->display);
      if (direction) g_signal_emit_by_name(self, "touch-navigation", direction);
      return TRUE;
    }
  }
  schedule_touch_frame(self->server);
  return TRUE; /* Do not also synthesize mouse clicks or GTK scrolling. */
}

void gnosis_compositor_view_set_navigation_gestures(GnosisCompositorView *self, gboolean enabled) {
  self->navigation_gestures = enabled;
  if (!enabled) self->edge_swipe.eligible = false;
}
