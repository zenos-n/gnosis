/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "browser.hpp"
#include <adwaita.h>
#include <dlfcn.h>
#include <unistd.h>
#include <cmath>
#include <cstring>
#include <cstdlib>

static std::shared_ptr<GnosisBrave::Page> page;
static GtkWidget *content;
static GDBusConnection *bus;
static bool busy;
static unsigned stage;
static gint64 started, scale_changed;
static const double scales[] = {1, 5.0 / 3, 2, 1.25, 1};
static void fail(const char *message) { g_printerr("FRACTIONAL_SCALE_FAIL %s\n", message); _exit(77); }
static GtkWidget *find_content(GtkWidget *widget) {
  if (!strcmp(G_OBJECT_TYPE_NAME(widget), "GnosisCompositorView")) return widget;
  for (auto *child = gtk_widget_get_first_child(widget); child; child = gtk_widget_get_next_sibling(child))
    if (auto *result = find_content(child)) return result;
  return nullptr;
}
static GdkTexture *largest_texture(GskRenderNode *node, double *width, double *height) {
  if (!node) return nullptr;
  switch (gsk_render_node_get_node_type(node)) {
    case GSK_TEXTURE_NODE:
    case GSK_TEXTURE_SCALE_NODE: {
      graphene_rect_t bounds; gsk_render_node_get_bounds(node, &bounds);
      *width = bounds.size.width; *height = bounds.size.height;
      return GDK_TEXTURE(g_object_ref(gsk_render_node_get_node_type(node) == GSK_TEXTURE_NODE ?
        gsk_texture_node_get_texture(node) : gsk_texture_scale_node_get_texture(node)));
    }
    case GSK_SUBSURFACE_NODE: return largest_texture(gsk_subsurface_node_get_child(node), width, height);
    case GSK_TRANSFORM_NODE: {
      auto *texture = largest_texture(gsk_transform_node_get_child(node), width, height);
      float xx, yx, xy, yy, dx, dy;
      gsk_transform_to_2d(gsk_transform_node_get_transform(node), &xx, &yx, &xy, &yy, &dx, &dy);
      *width *= std::abs(xx); *height *= std::abs(yy);
      return texture;
    }
    case GSK_CLIP_NODE: return largest_texture(gsk_clip_node_get_child(node), width, height);
    case GSK_ROUNDED_CLIP_NODE: return largest_texture(gsk_rounded_clip_node_get_child(node), width, height);
    case GSK_DEBUG_NODE: return largest_texture(gsk_debug_node_get_child(node), width, height);
    case GSK_CONTAINER_NODE: {
      GdkTexture *largest = nullptr;
      for (guint i = 0; i < gsk_container_node_get_n_children(node); i++) {
        double w = 0, h = 0;
        auto *texture = largest_texture(gsk_container_node_get_child(node, i), &w, &h);
        if (!texture) continue;
        if (!largest || gdk_texture_get_width(texture) * gdk_texture_get_height(texture) >
                        gdk_texture_get_width(largest) * gdk_texture_get_height(largest)) {
          g_clear_object(&largest); largest = texture; *width = w; *height = h;
        } else g_object_unref(texture);
      }
      return largest;
    }
    default: return nullptr;
  }
}
static void change_scale() {
  busy = true; scale_changed = 0;
  g_dbus_connection_call(bus, "org.gnome.Mutter.DisplayConfig", "/org/gnome/Mutter/DisplayConfig",
    "org.gnome.Mutter.DisplayConfig", "GetCurrentState", nullptr, nullptr, G_DBUS_CALL_FLAGS_NONE, 5000, nullptr,
    +[](GObject *source, GAsyncResult *result, gpointer) {
      GError *error = nullptr;
      auto *state = g_dbus_connection_call_finish(G_DBUS_CONNECTION(source), result, &error);
      if (!state) fail(error->message);
      auto *serial = g_variant_get_child_value(state, 0);
      auto *monitors = g_variant_get_child_value(state, 1);
      if (g_variant_n_children(monitors) != 1) fail("expected one isolated virtual monitor");
      auto *monitor = g_variant_get_child_value(monitors, 0);
      auto *spec = g_variant_get_child_value(monitor, 0);
      auto *modes = g_variant_get_child_value(monitor, 1);
      auto *mode = g_variant_get_child_value(modes, 0);
      const char *connector, *mode_id;
      g_variant_get_child(spec, 0, "&s", &connector); g_variant_get_child(mode, 0, "&s", &mode_id);
      if (!g_str_has_prefix(connector, "Meta-")) fail("refusing to configure a physical monitor");
      GVariantBuilder physical, logical, properties;
      g_variant_builder_init(&physical, G_VARIANT_TYPE("a(ssa{sv})"));
      g_variant_builder_add(&physical, "(ss@a{sv})", connector, mode_id,
        g_variant_new_array(G_VARIANT_TYPE("{sv}"), nullptr, 0));
      g_variant_builder_init(&logical, G_VARIANT_TYPE("a(iiduba(ssa{sv}))"));
      g_variant_builder_add(&logical, "(iidub@a(ssa{sv}))", 0, 0, scales[stage], 0u, TRUE, g_variant_builder_end(&physical));
      g_variant_builder_init(&properties, G_VARIANT_TYPE("a{sv}"));
      g_variant_builder_add(&properties, "{sv}", "layout-mode", g_variant_new_uint32(1));
      g_dbus_connection_call(bus, "org.gnome.Mutter.DisplayConfig", "/org/gnome/Mutter/DisplayConfig",
        "org.gnome.Mutter.DisplayConfig", "ApplyMonitorsConfig",
        g_variant_new("(uu@a(iiduba(ssa{sv}))@a{sv})", g_variant_get_uint32(serial), 1u,
          g_variant_builder_end(&logical), g_variant_builder_end(&properties)), nullptr,
        G_DBUS_CALL_FLAGS_NONE, 5000, nullptr, +[](GObject *source, GAsyncResult *result, gpointer) {
          GError *error = nullptr;
          auto *reply = g_dbus_connection_call_finish(G_DBUS_CONNECTION(source), result, &error);
          if (!reply) fail(error->message);
          g_variant_unref(reply); busy = false;
        }, nullptr);
      g_variant_unref(mode); g_variant_unref(modes); g_variant_unref(spec);
      g_variant_unref(monitor); g_variant_unref(monitors); g_variant_unref(serial); g_variant_unref(state);
    }, nullptr);
}
static gboolean tick(gpointer) {
  if (stage == G_N_ELEMENTS(scales)) return G_SOURCE_REMOVE;
  if (g_get_monotonic_time() - started > 90000000) fail("timeout");
  if (busy || page->loading || page->session.empty() || page->uri.find("fractional.html") == std::string::npos) return G_SOURCE_CONTINUE;
  auto *surface = gtk_native_get_surface(gtk_widget_get_native(content));
  if (std::abs(gdk_surface_get_scale(surface) - scales[stage]) > .001) return G_SOURCE_CONTINUE;
  if (!scale_changed) scale_changed = g_get_monotonic_time();
  if (g_get_monotonic_time() - scale_changed > 10000000) fail("renderer did not adopt the native window scale");
  busy = true;
  page->command("Runtime.evaluate", "{\"expression\":\"({dpr:devicePixelRatio,width:innerWidth,height:innerHeight})\",\"returnByValue\":true}",
    +[](JsonObject *result, const std::string &error) {
      busy = false;
      auto *remote = result ? json_object_get_object_member(result, "result") : nullptr;
      auto *value = remote ? json_object_get_object_member(remote, "value") : nullptr;
      if (!error.empty() || !value) return;
      double dpr = json_object_get_double_member_with_default(value, "dpr", 0);
      if (std::abs(dpr - scales[stage]) > .001) return;
      auto *paintable = gtk_widget_paintable_new(content); auto *snapshot = gtk_snapshot_new();
      int width = gtk_widget_get_width(content), height = gtk_widget_get_height(content);
      gdk_paintable_snapshot(paintable, GDK_SNAPSHOT(snapshot), width, height);
      double extent_w = 0, extent_h = 0;
      auto *node = gtk_snapshot_free_to_node(snapshot); auto *texture = largest_texture(node, &extent_w, &extent_h);
      if (node) gsk_render_node_unref(node); g_object_unref(paintable);
      if (!texture) return;
      int pixels_w = gdk_texture_get_width(texture), pixels_h = gdk_texture_get_height(texture);
      g_object_unref(texture);
      // Chromium can reuse an oversized GPU allocation; measure its sampled density.
      if (extent_w <= 0 || extent_h <= 0 || std::abs(pixels_w / extent_w - dpr) > .005 ||
          std::abs(pixels_h / extent_h - dpr) > .005) return;
      if (std::abs(json_object_get_double_member(value, "width") - width) > 1 ||
          std::abs(json_object_get_double_member(value, "height") - height) > 1) fail("logical viewport changed with raster scale");
      double latency = (g_get_monotonic_time() - scale_changed) / 1000.0;
      g_printerr("FRACTIONAL_SCALE_STEP scale=%.6f dpr=%.6f logical=%dx%d actual-buffer=%dx%d density=%.4fx%.4f latency=%.1fms\n",
        scales[stage], dpr, width, height, pixels_w, pixels_h, pixels_w / extent_w, pixels_h / extent_h, latency);
      if (stage && latency >= 400) fail("scale change exceeded400ms");
      if (++stage < G_N_ELEMENTS(scales)) change_scale();
      else {
        g_printerr("FRACTIONAL_SCALE_PASS actual raster density and live fractional/integer transitions\n");
        page.reset(); g_action_group_activate_action(G_ACTION_GROUP(g_application_get_default()), "quit", nullptr);
      }
    });
  return G_SOURCE_CONTINUE;
}
static gboolean start(gpointer) {
  auto *window = gtk_application_get_active_window(GTK_APPLICATION(g_application_get_default()));
  if (!window) return G_SOURCE_CONTINUE;
  auto embed = reinterpret_cast<void *(*)(void *)>(dlsym(RTLD_DEFAULT, "ephy_window_get_active_embed"));
  auto view = reinterpret_cast<void *(*)(void *)>(dlsym(RTLD_DEFAULT, "ephy_embed_get_web_view"));
  auto get_page = reinterpret_cast<std::shared_ptr<GnosisBrave::Page>(*)(void *)>(dlsym(RTLD_DEFAULT, "_Z27ephy_engine_view_brave_pageP15_EphyEngineView"));
  if (!embed || !view || !get_page) fail("missing accessors");
  page = get_page(view(embed(window))); content = page ? find_content(page->widget()) : nullptr;
  if (!content) return G_SOURCE_CONTINUE;
  GError *error = nullptr; bus = g_bus_get_sync(G_BUS_TYPE_SESSION, nullptr, &error);
  if (!bus) fail(error->message);
  gtk_window_set_default_size(window, 900, 650);
  started = g_get_monotonic_time(); change_scale(); g_timeout_add(30, tick, nullptr);
  return G_SOURCE_REMOVE;
}
__attribute__((constructor)) static void setup() {
  char exe[2048]; auto size = readlink("/proc/self/exe", exe, sizeof(exe) - 1);
  if (size < 0 || !std::getenv("GNOSIS_FRACTIONAL_TEST")) return;
  exe[size] = 0;
  if (strstr(exe, "/.gnosis-wrapped")) g_timeout_add_seconds(3, start, nullptr);
}
