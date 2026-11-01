/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "browser.hpp"
#include <adwaita.h>
#include <dlfcn.h>
#include <unistd.h>
#include <cstring>
#include <vector>

static GtkWindow* window;
static GtkWidget *button, *popover;
static std::vector<std::string> names{"uBlock Origin", "uBlock Origin"};
static size_t popup_index;
static int stage;
static bool busy, installed;
static gint64 started, changed;

static void fail(const char* message) {
  g_printerr("EXTENSION_POPUP_FAIL stage=%d popup_index=%zu %s\n", stage, popup_index, message);
  _exit(77);
}
static bool has_label(GtkWidget* widget, const char* text) {
  if (GTK_IS_LABEL(widget) && !g_strcmp0(gtk_label_get_text(GTK_LABEL(widget)), text)) return true;
  for (auto* child = gtk_widget_get_first_child(widget); child; child = gtk_widget_get_next_sibling(child))
    if (has_label(child, text)) return true;
  return false;
}
static GtkWidget* find(GtkWidget* widget, const char* type, const char* text = nullptr) {
  if (!strcmp(G_OBJECT_TYPE_NAME(widget), type) && (!text || has_label(widget, text))) return widget;
  for (auto* child = gtk_widget_get_first_child(widget); child; child = gtk_widget_get_next_sibling(child))
    if (auto* found = find(child, type, text)) return found;
  return nullptr;
}
static GtkWidget* extensions_button(GtkWidget* widget) {
  if (GTK_IS_MENU_BUTTON(widget) && !g_strcmp0(gtk_widget_get_tooltip_text(widget), "Extensions")) return widget;
  for (auto* child = gtk_widget_get_first_child(widget); child; child = gtk_widget_get_next_sibling(child))
    if (auto* found = extensions_button(child)) return found;
  return nullptr;
}
static std::shared_ptr<GnosisBrave::Page> page_for(void* view) {
  auto get = reinterpret_cast<std::shared_ptr<GnosisBrave::Page>(*)(void*)>(
    dlsym(RTLD_DEFAULT, "_Z27ephy_engine_view_brave_pageP15_EphyEngineView"));
  return get(view);
}
static void capture() {
  auto* paintable = gtk_widget_paintable_new(popover);
  auto* snapshot = gtk_snapshot_new();
  gdk_paintable_snapshot(paintable, GDK_SNAPSHOT(snapshot), gtk_widget_get_width(popover), gtk_widget_get_height(popover));
  auto* node = gtk_snapshot_free_to_node(snapshot);
  if (!node) fail("popup has no rendered content");
  auto* renderer = gsk_renderer_new_for_surface(gtk_native_get_surface(gtk_widget_get_native(popover)));
  auto* texture = gsk_renderer_render_texture(renderer, node, nullptr);
  const auto path = "/tmp/gnosis-extension-popup-" + std::to_string(popup_index) + ".png";
  if (!gdk_texture_save_to_png(texture, path.c_str())) fail("popup capture failed");
  g_object_unref(texture); gsk_renderer_unrealize(renderer); g_object_unref(renderer);
  gsk_render_node_unref(node); g_object_unref(paintable);
}
static gboolean tick(gpointer) {
  if (g_get_monotonic_time() - started > 70 * G_USEC_PER_SEC) fail("timeout");
  if (busy) return G_SOURCE_CONTINUE;
  auto* dialog = adw_application_window_get_visible_dialog(ADW_APPLICATION_WINDOW(window));
  if (dialog && ADW_IS_ALERT_DIALOG(dialog) && strstr(adw_alert_dialog_get_heading(ADW_ALERT_DIALOG(dialog)), "Default Browser")) {
    if (auto* no = find(GTK_WIDGET(dialog), "GtkButton", "No")) gtk_widget_activate(no);
    return G_SOURCE_CONTINUE;
  }
  if (stage == 0) {
    if (const char* extension = g_getenv("GNOSIS_POPUP_TEST_EXTENSION"); extension && !installed) {
      busy = GnosisBrave::extensions_load(extension, [](JsonObject*, const std::string& error) {
        busy = false;
        if (!error.empty()) fail(error.c_str());
        installed = true;
        names.push_back("Consent-O-Matic"); names.push_back("Consent-O-Matic");
      });
      return G_SOURCE_CONTINUE;
    }
    button = extensions_button(GTK_WIDGET(window));
    if (!button || !gtk_widget_get_visible(button)) return G_SOURCE_CONTINUE;
    gtk_menu_button_popup(GTK_MENU_BUTTON(button));
    popover = GTK_WIDGET(gtk_menu_button_get_popover(GTK_MENU_BUTTON(button)));
    stage = 1;
  } else if (stage == 1) {
    auto* row = find(popover, "EphyBrowserActionRow", names[popup_index].c_str());
    if (!row || !gtk_widget_is_sensitive(row) || !gtk_widget_get_mapped(row) ||
        find(popover, "EphyEngineView")) return G_SOURCE_CONTINUE;
    g_signal_emit_by_name(gtk_widget_get_parent(row), "row-activated", row);
    changed = g_get_monotonic_time(); stage = 2;
  } else if (stage == 2) {
    auto* view = find(popover, "EphyEngineView");
    if (!view) return G_SOURCE_CONTINUE;
    auto page = page_for(view);
    if (!page || page->session.empty()) {
      if (g_get_monotonic_time() - changed > 15 * G_USEC_PER_SEC) fail("mapped popup never started its browser session");
      return G_SOURCE_CONTINUE;
    }
    if (!gtk_widget_get_mapped(view) || gtk_widget_get_opacity(view) != 1 ||
        gtk_widget_get_height(view) <= 80) return G_SOURCE_CONTINUE;
    const auto script = std::string("document.readyState==='complete' && location.protocol==='chrome-extension:' && "
      "typeof chrome.runtime.getManifest==='function' && document.body.innerText.length>20") +
      (names[popup_index] == "uBlock Origin" ? " && !document.body.classList.contains('loading') && document.querySelector('#hostname')?.innerText.replace(/[\\s\\u00ad]/g,'').includes('127.0.0.1')" :
        " && document.querySelector('#site')?.textContent.startsWith('127.0.0.1:')");
    busy = true;
    page->command("Runtime.evaluate", "{\"expression\":" + GnosisBrave::quote(script) + ",\"returnByValue\":true}",
      [](JsonObject* result, const std::string& error) {
        busy = false;
        auto* value = result ? json_object_get_object_member(result, "result") : nullptr;
        if (!error.empty() || !value || !json_object_get_boolean_member_with_default(value, "value", false)) return;
        g_printerr("EXTENSION_POPUP_READY %s open=%zu\n", names[popup_index].c_str(), popup_index);
        stage = 3; changed = g_get_monotonic_time();
      });
  } else if (stage == 3 && g_get_monotonic_time() - changed > 300000) {
    capture();
    // Back must dispose the old view; reopening exercises a fresh unparented view.
    GtkWidget* back = nullptr;
    auto search = [&](auto&& self, GtkWidget* widget) -> void {
      if (GTK_IS_BUTTON(widget) && !g_strcmp0(gtk_widget_get_tooltip_text(widget), "Back to Extensions")) back = widget;
      for (auto* child = gtk_widget_get_first_child(widget); child; child = gtk_widget_get_next_sibling(child)) self(self, child);
    };
    search(search, popover);
    if (!back || !gtk_widget_activate(back)) fail("Back to Extensions unavailable");
    if (++popup_index == names.size()) {
      g_printerr("EXTENSION_POPUP_PASS real action pages, sizing, background readiness and reopen\n");
      gtk_menu_button_popdown(GTK_MENU_BUTTON(button));
      g_action_group_activate_action(G_ACTION_GROUP(g_application_get_default()), "quit", nullptr);
      return G_SOURCE_REMOVE;
    }
    stage = 1;
  }
  return G_SOURCE_CONTINUE;
}
static gboolean start(gpointer) {
  auto* app = g_application_get_default();
  if (!app || !GTK_IS_APPLICATION(app)) return G_SOURCE_CONTINUE;
  window = gtk_application_get_active_window(GTK_APPLICATION(app));
  if (!window) return G_SOURCE_CONTINUE;
  started = g_get_monotonic_time(); g_timeout_add(100, tick, nullptr);
  return G_SOURCE_REMOVE;
}
__attribute__((constructor)) static void setup() {
  char exe[2048]; auto size = readlink("/proc/self/exe", exe, sizeof(exe) - 1);
  if (size < 0) return;
  exe[size] = 0;
  if (strstr(exe, "/.gnosis-wrapped")) g_timeout_add_seconds(4, start, nullptr);
}
