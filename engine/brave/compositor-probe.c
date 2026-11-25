/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "compositor.h"
#include "profile.h"
#include <adwaita.h>
#include <sys/resource.h>

static GSubprocess *browser;
static const char *url;
static char *profile;
static void content_fullscreen(GtkWidget *content, gboolean fullscreen, GtkWidget *header) {
  (void)content; gtk_widget_set_visible(header, !fullscreen);
}
static void activate(GtkApplication *application, gpointer data) {
  (void)data; GError *error = NULL;
  GtkWidget *content = gnosis_compositor_view_new(&error);
  if (!content) { g_printerr("%s\n", error->message); g_error_free(error); g_application_quit(G_APPLICATION(application)); return; }
  GtkWidget *window = adw_application_window_new(application);
  gtk_window_set_title(GTK_WINDOW(window), "Gnosis — compositor prototype");
  gtk_window_set_default_size(GTK_WINDOW(window), 1100, 760);
  GtkWidget *toolbar = adw_toolbar_view_new();
  GtkWidget *header = adw_header_bar_new();
  adw_header_bar_set_title_widget(ADW_HEADER_BAR(header), adw_window_title_new("Gnosis", "Wayland surfaces inside GTK"));
  adw_toolbar_view_add_top_bar(ADW_TOOLBAR_VIEW(toolbar), header);
  g_signal_connect_object(content, "fullscreen-changed", G_CALLBACK(content_fullscreen), header, 0);
  GtkWidget *split = adw_overlay_split_view_new();
  adw_overlay_split_view_set_collapsed(ADW_OVERLAY_SPLIT_VIEW(split), TRUE);
  adw_overlay_split_view_set_show_sidebar(ADW_OVERLAY_SPLIT_VIEW(split), FALSE);
  adw_overlay_split_view_set_sidebar_position(ADW_OVERLAY_SPLIT_VIEW(split), GTK_PACK_END);
  adw_overlay_split_view_set_content(ADW_OVERLAY_SPLIT_VIEW(split), content);
  GtkWidget *sidebar = gtk_box_new(GTK_ORIENTATION_VERTICAL, 12);
  gtk_widget_set_margin_start(sidebar, 24); gtk_widget_set_margin_end(sidebar, 24);
  gtk_widget_set_margin_top(sidebar, 24); gtk_widget_set_margin_bottom(sidebar, 24);
  gtk_box_append(GTK_BOX(sidebar), gtk_label_new("GTK draws above Brave"));
  GtkWidget *entry = gtk_entry_new(); gtk_entry_set_placeholder_text(GTK_ENTRY(entry), "GTK keyboard focus");
  gtk_box_append(GTK_BOX(sidebar), entry);
  adw_overlay_split_view_set_sidebar(ADW_OVERLAY_SPLIT_VIEW(split), sidebar);
  GtkWidget *toggle = gtk_toggle_button_new(); gtk_button_set_icon_name(GTK_BUTTON(toggle), "sidebar-show-symbolic");
  g_object_bind_property(toggle, "active", split, "show-sidebar", G_BINDING_BIDIRECTIONAL | G_BINDING_SYNC_CREATE);
  adw_header_bar_pack_end(ADW_HEADER_BAR(header), toggle);
  adw_toolbar_view_set_content(ADW_TOOLBAR_VIEW(toolbar), split);
  adw_application_window_set_content(ADW_APPLICATION_WINDOW(window), toolbar);
  gtk_window_present(GTK_WINDOW(window));
  GSubprocessLauncher *launcher = g_subprocess_launcher_new(G_SUBPROCESS_FLAGS_NONE);
  g_subprocess_launcher_setenv(launcher, "WAYLAND_DISPLAY", gnosis_compositor_view_get_socket(GNOSIS_COMPOSITOR_VIEW(content)), TRUE);
  g_subprocess_launcher_unsetenv(launcher, "WAYLAND_SOCKET");
  g_subprocess_launcher_unsetenv(launcher, "DISPLAY");
  profile = g_dir_make_tmp("gnosis-compositor-brave-XXXXXX", NULL);
  if (!gnosis_brave_profile_prepare(profile, &error)) {
    g_warning("Brave profile: %s", error->message); g_error_free(error); g_object_unref(launcher); return;
  }
  char *user_data = g_strconcat("--user-data-dir=", profile, NULL);
  char *app = g_strconcat("--app=", url, NULL);
  const char *executable = g_getenv("GNOSIS_BRAVE_EXECUTABLE");
  browser = g_subprocess_launcher_spawn(launcher, &error, executable ? executable : "brave",
    user_data, app, "--ozone-platform=wayland", "--no-first-run", "--no-default-browser-check",
    "--disable-background-mode", "--password-store=basic", NULL);
  g_free(user_data); g_free(app); g_object_unref(launcher);
  if (!browser) { g_warning("Brave failed to start: %s", error->message); g_error_free(error); }
}
int main(int argc, char **argv) {
  struct rlimit no_core = {0, 0}; if (setrlimit(RLIMIT_CORE, &no_core)) return 1;
  g_setenv("GNOSIS_COLOR_MANAGEMENT", "1", TRUE);
  url = argc > 1 ? argv[1] : "https://browserbench.org/";
  AdwApplication *app = adw_application_new("com.neg-zero.Gnosis.CompositorProbe", G_APPLICATION_NON_UNIQUE);
  g_signal_connect(app, "activate", G_CALLBACK(activate), NULL);
  int result = g_application_run(G_APPLICATION(app), 1, argv);
  if (browser) { g_subprocess_send_signal(browser, SIGTERM); g_object_unref(browser); }
  g_object_unref(app); g_free(profile); return result;
}
