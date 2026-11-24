/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "browser.hpp"
#include <adwaita.h>
#include <gdk/x11/gdkx.h>
#include <dlfcn.h>
#include <unistd.h>
#include <cstring>
#include <vector>

static GtkWindow *window;
static AdwTabView *tabs;
static GSettings *settings;
static AdwTabPage *pages[4];
static std::shared_ptr<GnosisBrave::Page> native[4];
static int stage, selection_changes;
static gint64 started, changed, clicked;
static AdwSwitchRow *preference;
static bool track_selection, busy;
static void fail(const char *message) {
  g_printerr("TAB_CLOSE_FOCUS_FAIL stage=%d %s\n", stage, message); _exit(77);
}
static GtkWidget *find(GtkWidget *widget, const char *type, const char *text = nullptr) {
  if (!strcmp(G_OBJECT_TYPE_NAME(widget), type) && (!text ||
      (GTK_IS_LABEL(widget) && !strcmp(gtk_label_get_text(GTK_LABEL(widget)), text)) ||
      (ADW_IS_PREFERENCES_ROW(widget) && !strcmp(adw_preferences_row_get_title(ADW_PREFERENCES_ROW(widget)), text))))
    return widget;
  for (auto *child = gtk_widget_get_first_child(widget); child; child = gtk_widget_get_next_sibling(child))
    if (auto *found = find(child, type, text)) return found;
  return nullptr;
}
static auto active_native() {
  auto embed = reinterpret_cast<void *(*)(void *)>(dlsym(RTLD_DEFAULT, "ephy_window_get_active_embed"))(window);
  auto view = reinterpret_cast<void *(*)(void *)>(dlsym(RTLD_DEFAULT, "ephy_embed_get_web_view"))(embed);
  return reinterpret_cast<std::shared_ptr<GnosisBrave::Page>(*)(void *)>(dlsym(RTLD_DEFAULT, "_Z27ephy_engine_view_brave_pageP15_EphyEngineView"))(view);
}
static void make_tabs() {
  pages[0] = adw_tab_view_get_selected_page(tabs); native[0] = active_native();
  for (int i = 1; i < 4; ++i) {
    gtk_widget_activate_action(GTK_WIDGET(window), "win.new-tab", nullptr);
    pages[i] = adw_tab_view_get_selected_page(tabs); native[i] = active_native();
    native[i]->navigate(native[0]->uri + "?tab=" + std::to_string(i));
  }
}
static void click_tab(int index) {
  auto *bar = find(GTK_WIDGET(window), "AdwTabBar");
  auto *label = find(bar, "GtkLabel", ("Fixture tab " + std::to_string(index)).c_str());
  if (!label || !gtk_widget_get_mapped(label)) fail("tab label not mapped");
  graphene_point_t point{float(gtk_widget_get_width(label) / 2), float(gtk_widget_get_height(label) / 2)}, mapped;
  if (!gtk_widget_compute_point(label, GTK_WIDGET(window), &point, &mapped)) fail("tab coordinate mapping");
  auto id = std::to_string(gdk_x11_surface_get_xid(gtk_native_get_surface(GTK_NATIVE(window))));
  std::vector<std::string> args{g_getenv("GNOSIS_XDOTOOL"), "windowfocus", id, "mousemove", "--window", id,
    std::to_string(int(mapped.x)), std::to_string(int(mapped.y)), "click", "2"};
  std::vector<char *> argv; for (auto &arg : args) argv.push_back(arg.data()); argv.push_back(nullptr);
  int status = 0; clicked = g_get_monotonic_time();
  if (!g_spawn_sync(nullptr, argv.data(), nullptr, G_SPAWN_DEFAULT, nullptr, nullptr, nullptr, nullptr, &status, nullptr) || status)
    fail("XTest middle click");
}
static bool closed(int index) { return native[index]->is_closed(); }
static void check_selected(int index, bool unchanged = true) {
  if (adw_tab_view_get_selected_page(tabs) != pages[index]) fail("wrong selected tab");
  if (unchanged && selection_changes) fail("background close temporarily stole selection");
}
static void next() { ++stage; changed = g_get_monotonic_time(); }
static gboolean tick(gpointer) {
  auto now = g_get_monotonic_time();
  if (now - started > 50 * G_USEC_PER_SEC) fail("timeout");
  if (busy || now - changed < 250000) return G_SOURCE_CONTINUE;
  auto *dialog = adw_application_window_get_visible_dialog(ADW_APPLICATION_WINDOW(window));
  if (dialog && ADW_IS_ALERT_DIALOG(dialog) && stage != 11) { adw_dialog_force_close(dialog); return G_SOURCE_CONTINUE; }
  if (stage == 0) {
    for (auto &page : native) if (page->session.empty() || page->loading || page->uri.find("fixture") == std::string::npos) return G_SOURCE_CONTINUE;
    for (int i = 0; i < 4; ++i) adw_tab_page_set_title(pages[i], ("Fixture tab " + std::to_string(i)).c_str());
    adw_tab_view_set_selected_page(tabs, pages[1]); next();
  } else if (stage == 1) {
    selection_changes = 0; track_selection = true; click_tab(3); next();
  } else if (stage == 2) {
    if (!closed(3) || adw_tab_view_get_n_pages(tabs) != 3) return G_SOURCE_CONTINUE;
    check_selected(1); g_print("TAB_CLOSE_FOCUS_RIGHT_PASS\n"); click_tab(0); next();
  } else if (stage == 3) {
    if (!closed(0) || adw_tab_view_get_n_pages(tabs) != 2) return G_SOURCE_CONTINUE;
    check_selected(1); g_print("TAB_CLOSE_FOCUS_LEFT_PASS\n"); track_selection = false; click_tab(1); next();
  } else if (stage == 4) {
    if (!closed(1) || adw_tab_view_get_n_pages(tabs) != 1) return G_SOURCE_CONTINUE;
    check_selected(2, false); g_print("TAB_CLOSE_FOCUS_SELECTED_PASS\n");
    native[0] = native[2]; pages[0] = pages[2];
    make_tabs(); next();
  } else if (stage == 5) {
    for (auto &page : native) if (page->session.empty() || page->loading) return G_SOURCE_CONTINUE;
    for (int i = 0; i < 4; ++i) adw_tab_page_set_title(pages[i], ("Fixture tab " + std::to_string(i)).c_str());
    gtk_widget_activate_action(GTK_WIDGET(window), "app.preferences", nullptr); next();
  } else if (stage == 6) {
    if (!dialog || !ADW_IS_PREFERENCES_DIALOG(dialog)) return G_SOURCE_CONTINUE;
    preference = ADW_SWITCH_ROW(find(GTK_WIDGET(dialog), "AdwSwitchRow", "Keep Current Tab When Closing _Other Tabs"));
    if (!preference || !adw_switch_row_get_active(preference)) fail("default-on native preference missing");
    adw_switch_row_set_active(preference, false);
    if (g_settings_get_boolean(settings, "keep-current-tab-on-close")) fail("preference did not update setting");
    adw_dialog_close(dialog); next();
  } else if (stage == 7) {
    if (dialog) return G_SOURCE_CONTINUE;
    adw_tab_view_set_selected_page(tabs, pages[0]); click_tab(3); next();
  } else if (stage == 8) {
    if (!closed(3) || adw_tab_view_get_n_pages(tabs) != 3) return G_SOURCE_CONTINUE;
    if (adw_tab_view_get_selected_page(tabs) == pages[0]) fail("disabled setting did not restore old behavior");
    g_print("TAB_CLOSE_FOCUS_DISABLED_PASS native preference binding\n");
    g_settings_set_boolean(settings, "keep-current-tab-on-close", true);
    adw_tab_view_set_selected_page(tabs, pages[0]);
    adw_tab_view_close_page(tabs, pages[2]);
    adw_tab_view_set_selected_page(tabs, pages[1]); selection_changes = 0; track_selection = true; next();
  } else if (stage == 9) {
    if (!closed(2) || adw_tab_view_get_n_pages(tabs) != 2) return G_SOURCE_CONTINUE;
    check_selected(1); g_print("TAB_CLOSE_FOCUS_PENDING_SWITCH_PASS\n");
    native[0]->command("Input.dispatchMouseEvent", "{\"type\":\"mousePressed\",\"button\":\"left\",\"buttons\":1,\"x\":25,\"y\":25,\"clickCount\":1}");
    native[0]->command("Input.dispatchMouseEvent", "{\"type\":\"mouseReleased\",\"button\":\"left\",\"buttons\":0,\"x\":25,\"y\":25,\"clickCount\":1}");
    busy = true;
    native[0]->command("Runtime.evaluate", "{\"expression\":\"window.onbeforeunload=e=>{e.preventDefault();e.returnValue=''}\"}", [](JsonObject *reply, const std::string &error) {
      if (!reply || !error.empty()) fail("arm background beforeunload"); busy = false;
    }); next();
  } else if (stage == 10) {
    selection_changes = 0; click_tab(0); next();
  } else if (stage == 11) {
    if (!dialog || !ADW_IS_ALERT_DIALOG(dialog)) return G_SOURCE_CONTINUE;
    if (strcmp(adw_alert_dialog_get_heading(ADW_ALERT_DIALOG(dialog)), "Leave This Page?")) fail("missing native close warning");
    check_selected(1);
    auto *label = find(GTK_WIDGET(dialog), "GtkLabel", "Keep Open");
    auto *button = label ? gtk_widget_get_ancestor(label, GTK_TYPE_BUTTON) : nullptr;
    if (!button) fail("Keep Open button missing"); gtk_widget_activate(button); next();
  } else if (stage == 12) {
    if (dialog) return G_SOURCE_CONTINUE;
    if (closed(0) || adw_tab_view_get_n_pages(tabs) != 2) fail("canceled background close lost a tab");
    check_selected(1); g_print("TAB_CLOSE_FOCUS_CANCEL_PASS native beforeunload, no selection change\n");
    busy = true;
    native[0]->command("Runtime.evaluate", "{\"expression\":\"window.onbeforeunload=null\"}", [](JsonObject *reply, const std::string &error) {
      if (!reply || !error.empty()) fail("clear beforeunload"); busy = false;
    }); next();
  } else if (stage == 13) {
    track_selection = false;
    for (auto &page : native) page.reset();
    g_print("TAB_CLOSE_FOCUS_PASS real middle click, left/right/selected, default-on setting, disabled behavior and selection during close\n");
    g_action_group_activate_action(G_ACTION_GROUP(g_application_get_default()), "quit", nullptr); return G_SOURCE_REMOVE;
  }
  return G_SOURCE_CONTINUE;
}
static gboolean start(gpointer) {
  auto *app = g_application_get_default(); if (!app) return G_SOURCE_CONTINUE;
  window = gtk_application_get_active_window(GTK_APPLICATION(app)); if (!window) return G_SOURCE_CONTINUE;
  tabs = ADW_TAB_VIEW(find(GTK_WIDGET(window), "AdwTabView")); if (!tabs) return G_SOURCE_CONTINUE;
  settings = reinterpret_cast<GSettings *(*)(const char *)>(dlsym(RTLD_DEFAULT, "ephy_settings_get"))("com.neg-zero.Gnosis");
  if (!g_settings_get_boolean(settings, "keep-current-tab-on-close")) fail("schema default is not enabled");
  g_settings_set_boolean(settings, "ask-for-default", false);
  g_settings_set_boolean(settings, "warn-on-close-unsubmitted-data", false);
  g_signal_connect(tabs, "notify::selected-page", G_CALLBACK(+[](GObject *, GParamSpec *, gpointer) {
    if (track_selection) ++selection_changes;
  }), nullptr);
  g_signal_connect(tabs, "page-detached", G_CALLBACK(+[](AdwTabView *, AdwTabPage *, int, gpointer) {
    if (clicked && (stage == 2 || stage == 3 || stage == 4)) {
      double latency = (g_get_monotonic_time() - clicked) / 1000.;
      g_print("TAB_CLOSE_FOCUS_FEEDBACK_MS %.1f\n", latency);
      if (latency > 400) fail("close feedback exceeded 400ms");
    }
  }), nullptr);
  make_tabs(); started = changed = g_get_monotonic_time(); g_timeout_add(20, tick, nullptr); return G_SOURCE_REMOVE;
}
__attribute__((constructor)) static void setup() {
  char exe[2048]; auto n = readlink("/proc/self/exe", exe, sizeof(exe) - 1); if (n < 0) return; exe[n] = 0;
  if (strstr(exe, "/.gnosis-wrapped")) g_timeout_add_seconds(3, start, nullptr);
}
