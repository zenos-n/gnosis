/* SPDX-License-Identifier: GPL-3.0-or-later */
static void monitor_output_free(gpointer data) {
  MonitorOutput *monitor = data;
  g_clear_object(&monitor->monitor);
  g_free(monitor);
}
static void update_monitor_output(GnosisCompositor *server, MonitorOutput *monitor) {
  GdkRectangle geometry;
  gdk_monitor_get_geometry(monitor->monitor, &geometry);
  double scale = gdk_monitor_get_scale(monitor->monitor);
  if (!isfinite(scale) || scale <= 0) scale = 1;
  int width = MAX(1, (int)lround(geometry.width * scale));
  int height = MAX(1, (int)lround(geometry.height * scale));
  int refresh = gdk_monitor_get_refresh_rate(monitor->monitor);
  if (refresh <= 0) refresh = 60000;
  struct wlr_output *output = monitor->output;
  if (!output->enabled || output->width != width || output->height != height ||
      output->refresh != refresh || output->scale != (float)scale) {
    struct wlr_output_state state;
    wlr_output_state_init(&state);
    wlr_output_state_set_enabled(&state, TRUE);
    wlr_output_state_set_custom_mode(&state, width, height, refresh);
    wlr_output_state_set_scale(&state, scale);
    if (!wlr_output_commit_state(output, &state)) g_warning("Could not mirror GTK monitor mode");
    wlr_output_state_finish(&state);
  }
  struct wlr_output_layout_output *position = wlr_output_layout_get(server->output_layout, output);
  if (!position || position->x != geometry.x || position->y != geometry.y)
    wlr_output_layout_add(server->output_layout, output, geometry.x, geometry.y);
}
static MonitorOutput *monitor_output(GnosisCompositor *server, GdkMonitor *gdk_monitor) {
  if (!gdk_monitor) return NULL;
  for (guint i = 0; i < server->outputs->len; i++) {
    MonitorOutput *monitor = g_ptr_array_index(server->outputs, i);
    if (monitor->monitor == gdk_monitor) return monitor;
  }
  MonitorOutput *monitor = g_new0(MonitorOutput, 1);
  monitor->monitor = g_object_ref(gdk_monitor);
  monitor->output = wlr_headless_add_output(server->backend, 1280, 800);
  if (!monitor->output) { monitor_output_free(monitor); return NULL; }
  g_autofree char *name = g_strdup_printf("Gnosis-%s-%u", gdk_monitor_get_connector(gdk_monitor) ?: "display", server->outputs->len);
  wlr_output_set_name(monitor->output, name);
  monitor->output->phys_width = gdk_monitor_get_width_mm(gdk_monitor);
  monitor->output->phys_height = gdk_monitor_get_height_mm(gdk_monitor);
  update_monitor_output(server, monitor);
  wlr_output_create_global(monitor->output, server->display);
  g_ptr_array_add(server->outputs, monitor);
  return monitor;
}
static void enter_monitor_surface(struct wlr_surface *surface, int x, int y, void *data) {
  (void)x; (void)y; wlr_surface_send_enter(surface, ((MonitorOutput *)data)->output);
}
static void leave_monitor_surface(struct wlr_surface *surface, int x, int y, void *data) {
  (void)x; (void)y; wlr_surface_send_leave(surface, ((MonitorOutput *)data)->output);
}
static void sync_output_refresh(GnosisCompositor *server) {
  for (guint i = server->outputs->len; i > 0; i--) {
    MonitorOutput *monitor = g_ptr_array_index(server->outputs, i - 1);
    if (gdk_monitor_is_valid(monitor->monitor)) {
      update_monitor_output(server, monitor);
      continue;
    }
    View *view;
    wl_list_for_each(view, &server->views, link) if (view->monitor == monitor) {
      wlr_xdg_surface_for_each_surface(view->toplevel->base, leave_monitor_surface, monitor);
      view->monitor = NULL;
    }
    wlr_output_destroy(monitor->output);
    g_ptr_array_remove_index(server->outputs, i - 1);
  }
}
