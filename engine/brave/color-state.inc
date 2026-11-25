/* SPDX-License-Identifier: GPL-3.0-or-later */
static struct wlr_image_description_v1_data srgb_description(void) {
  return (struct wlr_image_description_v1_data){
    .tf_named = WP_COLOR_MANAGER_V1_TRANSFER_FUNCTION_SRGB,
    .primaries_named = WP_COLOR_MANAGER_V1_PRIMARIES_SRGB,
  };
}
static GdkColorState *surface_color(struct wlr_surface *surface) {
  const struct wlr_image_description_v1_data *description = wlr_surface_get_image_description_v1_data(surface);
  if (!description || (description->primaries_named == WP_COLOR_MANAGER_V1_PRIMARIES_SRGB &&
      description->tf_named == WP_COLOR_MANAGER_V1_TRANSFER_FUNCTION_SRGB))
    return gdk_color_state_ref(gdk_color_state_get_srgb());
  if (description->primaries_named == WP_COLOR_MANAGER_V1_PRIMARIES_BT2020 &&
      description->tf_named == WP_COLOR_MANAGER_V1_TRANSFER_FUNCTION_ST2084_PQ)
    return gdk_color_state_ref(gdk_color_state_get_rec2100_pq());
  g_autoptr(GdkCicpParams) params = gdk_cicp_params_new();
  gdk_cicp_params_set_color_primaries(params, description->primaries_named == WP_COLOR_MANAGER_V1_PRIMARIES_BT2020 ? 9 : 1);
  gdk_cicp_params_set_transfer_function(params, description->tf_named == WP_COLOR_MANAGER_V1_TRANSFER_FUNCTION_ST2084_PQ ? 16 : 13);
  gdk_cicp_params_set_range(params, GDK_CICP_RANGE_FULL);
  GdkColorState *color = gdk_cicp_params_build_color_state(params, NULL);
  return color ? color : gdk_color_state_ref(gdk_color_state_get_srgb());
}
static void color_surface(struct wlr_surface *surface, int x, int y, void *data) {
  (void)x; (void)y;
  View *view = data;
  wlr_color_manager_v1_set_surface_preferred_image_description(view->server->colors, surface, &view->color);
}
static void sync_view_color(View *view) {
  if (!view->host) return;
  GtkNative *native = gtk_widget_get_native(GTK_WIDGET(view->host));
  GdkSurface *surface = native ? gtk_native_get_surface(native) : NULL;
  struct wlr_image_description_v1_data color = srgb_description();
  if (surface && GDK_IS_WAYLAND_SURFACE(surface)) {
    guint32 luminances[5];
    GdkColorState *state = gnosis_gdk_wayland_surface_get_preferred(surface, luminances);
    if (gdk_color_state_equivalent(state, gdk_color_state_get_rec2100_pq())) {
      color.tf_named = WP_COLOR_MANAGER_V1_TRANSFER_FUNCTION_ST2084_PQ;
      color.primaries_named = WP_COLOR_MANAGER_V1_PRIMARIES_BT2020;
      gint32 primaries[8]; guint32 light[2];
      if (gnosis_gdk_wayland_surface_get_target(surface, primaries, light)) {
        color.has_mastering_display_primaries = TRUE;
        color.mastering_display_primaries = (struct wlr_color_primaries){
          .red = {primaries[0]/1000000.f, primaries[1]/1000000.f},
          .green = {primaries[2]/1000000.f, primaries[3]/1000000.f},
          .blue = {primaries[4]/1000000.f, primaries[5]/1000000.f},
          .white = {primaries[6]/1000000.f, primaries[7]/1000000.f},
        };
      }
      color.max_cll = light[0]; color.max_fall = light[1];
      if (luminances[1] && luminances[2]) {
        color.has_luminances = color.has_target_luminance = TRUE;
        color.luminances = (struct wlr_color_luminances){luminances[0] / 10000.f, luminances[1], luminances[2]};
        color.target_luminance = (struct wlr_color_luminances){luminances[3] / 10000.f, luminances[4], luminances[2]};
      }
    }
  }
  GdkMonitor *gdk_monitor = surface ? gdk_display_get_monitor_at_surface(gtk_widget_get_display(GTK_WIDGET(view->host)), surface) : NULL;
  MonitorOutput *monitor = monitor_output(view->server, gdk_monitor);
  gboolean moved = monitor != view->monitor;
  if (moved) {
    if (view->monitor) wlr_xdg_surface_for_each_surface(view->toplevel->base, leave_monitor_surface, view->monitor);
    view->monitor = monitor;
    if (monitor) wlr_xdg_surface_for_each_surface(view->toplevel->base, enter_monitor_surface, monitor);
  }
  if (!moved && wlr_image_description_v1_data_equal(&view->color, &color)) return;
  view->color = color;
  color_surface(view->toplevel->base->surface, 0, 0, view);
  wlr_xdg_surface_for_each_surface(view->toplevel->base, color_surface, view);
  if (monitor) wlr_color_manager_v1_set_output_image_description(view->server->colors, monitor->output, &color);
  g_message("Nested view color follows GTK output: %s", color.tf_named == WP_COLOR_MANAGER_V1_TRANSFER_FUNCTION_ST2084_PQ ? "BT.2020 PQ HDR" : "sRGB SDR");
}
static void release_surface_offload(struct wlr_surface *surface, int x, int y, void *data) {
  (void)x; (void)y; (void)data;
  Surface *state = surface->data;
  if (state) gnosis_gtk_browser_surface_release(&state->offload);
}
