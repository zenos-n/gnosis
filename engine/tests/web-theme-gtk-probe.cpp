/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "browser.hpp"
#include <adwaita.h>
#include <dlfcn.h>
#include <unistd.h>
#include <cstring>
#include <vector>

// Force an independent renderer for the localhost child in this test only.
extern "C" GSubprocess* g_subprocess_launcher_spawnv(GSubprocessLauncher* launcher, const gchar* const* argv, GError** error) {
  auto real = reinterpret_cast<GSubprocess*(*)(GSubprocessLauncher*, const gchar* const*, GError**)>(dlsym(RTLD_NEXT, "g_subprocess_launcher_spawnv"));
  if (!argv || !*argv || !strstr(*argv, "brave")) return real(launcher, argv, error);
  std::vector<const char*> args;
  for (auto p = argv; *p; ++p) args.push_back(*p);
  args.push_back("--site-per-process"); args.push_back(nullptr);
  return real(launcher, args.data(), error);
}

static std::shared_ptr<GnosisBrave::Page> regular, private_page, fresh;
static std::string fixture;
static gint64 started;
static int stage;
static bool busy;
static AdwColorScheme previous;
static std::vector<GtkWindow*> windows;

static gboolean tick(gpointer) {
  if (g_get_monotonic_time() - started > 60 * G_USEC_PER_SEC) {
    g_printerr("WEB_THEME_FAIL stage=%d\n", stage); _exit(77);
  }
  if (busy || regular->session.empty()) return G_SOURCE_CONTINUE;
  if (stage == 0) {
    auto* manager = adw_style_manager_get_default();
    previous = adw_style_manager_get_color_scheme(manager);
    adw_style_manager_set_color_scheme(manager, ADW_COLOR_SCHEME_FORCE_DARK);
    regular->navigate(fixture);
    auto create = [](bool ephemeral) {
      auto* window = GTK_WINDOW(gtk_window_new());
      windows.push_back(window);
      auto* content = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
      gtk_window_set_child(window, content);
      gtk_window_set_default_size(window, 400, 300);
      auto page = GnosisBrave::Page::create(content, ephemeral);
      gtk_window_present(window); page->start(); page->navigate(fixture);
      return page;
    };
    private_page = create(true);
    stage = 1;
    return G_SOURCE_CONTINUE;
  }
  auto page = stage == 1 || stage == 3 || stage == 5 ? regular : stage == 7 ? fresh : private_page;
  if (page->session.empty()) return G_SOURCE_CONTINUE;
  const bool dark = stage < 3 || stage == 5 || stage == 6;
  const bool initial = stage < 3 || stage == 7;
  const std::string expected = dark ? "true" : "false";
  const std::string color = dark ? "rgb(4, 5, 6)" : "rgb(1, 2, 3)";
  const auto script = "location.pathname.endsWith('/web-theme.html') && document.readyState==='complete' && "
    "typeof childTheme!=='undefined' && childTheme && media.matches===" + expected +
    " && childTheme.dark===" + expected + " && getComputedStyle(document.body).color===" + GnosisBrave::quote(color) +
    " && childTheme.color===" + GnosisBrave::quote(color) +
    (initial ? " && initialDark===" + expected + " && childTheme.initialDark===" + expected :
      " && themeChanges.at(-1)===" + expected + " && childTheme.changes.at(-1)===" + expected);
  busy = true;
  page->command("Runtime.evaluate", "{\"expression\":" + GnosisBrave::quote(script) + ",\"returnByValue\":true}",
    [](JsonObject* result, const std::string& error) {
      busy = false;
      auto* value = result ? json_object_get_object_member(result, "result") : nullptr;
      if (!error.empty() || !value || !json_object_get_boolean_member_with_default(value, "value", false)) return;
      g_printerr("WEB_THEME_STAGE_PASS %d\n", stage++);
      if (stage == 3) adw_style_manager_set_color_scheme(adw_style_manager_get_default(), ADW_COLOR_SCHEME_FORCE_LIGHT);
      if (stage == 5) adw_style_manager_set_color_scheme(adw_style_manager_get_default(), ADW_COLOR_SCHEME_FORCE_DARK);
      if (stage == 7) {
        adw_style_manager_set_color_scheme(adw_style_manager_get_default(), ADW_COLOR_SCHEME_FORCE_LIGHT);
        auto* window = GTK_WINDOW(gtk_window_new());
        windows.push_back(window);
        auto* content = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
        gtk_window_set_child(window, content);
        fresh = GnosisBrave::Page::create(content, false);
        gtk_window_present(window); fresh->start(); fresh->navigate(fixture);
      }
      if (stage == 8) {
        g_printerr("WEB_THEME_PASS initial dark/light, CSS, matchMedia change events, private tabs and cross-process frames\n");
        adw_style_manager_set_color_scheme(adw_style_manager_get_default(), previous);
        private_page->close(); fresh->close(); regular.reset(); private_page.reset(); fresh.reset();
        for (auto* window : windows) gtk_window_destroy(window);
        g_action_group_activate_action(G_ACTION_GROUP(g_application_get_default()), "quit", nullptr);
      }
    });
  return stage == 8 ? G_SOURCE_REMOVE : G_SOURCE_CONTINUE;
}

static gboolean start(gpointer) {
  auto* app = g_application_get_default();
  if (!app || !GTK_IS_APPLICATION(app)) return G_SOURCE_CONTINUE;
  auto* window = gtk_application_get_active_window(GTK_APPLICATION(app));
  if (!window) return G_SOURCE_CONTINUE;
  auto embed = reinterpret_cast<void*(*)(void*)>(dlsym(RTLD_DEFAULT, "ephy_window_get_active_embed"));
  auto view = reinterpret_cast<void*(*)(void*)>(dlsym(RTLD_DEFAULT, "ephy_embed_get_web_view"));
  auto page = reinterpret_cast<std::shared_ptr<GnosisBrave::Page>(*)(void*)>(dlsym(RTLD_DEFAULT, "_Z27ephy_engine_view_brave_pageP15_EphyEngineView"));
  if (!embed(window)) return G_SOURCE_CONTINUE;
  regular = page(view(embed(window)));
  const char* url = g_getenv("GNOSIS_THEME_FIXTURE");
  fixture = url ? url : "http://127.0.0.1:18473/web-theme.html";
  started = g_get_monotonic_time();
  g_timeout_add(100, tick, nullptr);
  return G_SOURCE_REMOVE;
}
__attribute__((constructor)) static void setup() {
  char exe[2048]; auto size = readlink("/proc/self/exe", exe, sizeof(exe) - 1);
  if (size < 0) return;
  exe[size] = 0;
  if (strstr(exe, "/.gnosis-wrapped")) g_timeout_add_seconds(2, start, nullptr);
}
