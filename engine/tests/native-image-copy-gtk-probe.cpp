/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "browser.hpp"
#include <adwaita.h>
#include <gdk/x11/gdkx.h>
#include <dlfcn.h>
#include <unistd.h>
#include <cstring>
#include <vector>

static GtkWindow *window;
static GtkWidget *host;
static std::shared_ptr<GnosisBrave::Page> page;
static std::string origin;
static int stage, scenario;
static bool busy;
static gint64 started, clicked;
static void fail(const char *message) {
  g_printerr("IMAGE_COPY_FAIL case=%d stage=%d %s\n", scenario, stage, message);
  _exit(77);
}
static bool has_label(GtkWidget *widget, const char *label) {
  if (GTK_IS_LABEL(widget) && !strcmp(gtk_label_get_text(GTK_LABEL(widget)), label)) return true;
  for (auto *child=gtk_widget_get_first_child(widget); child; child=gtk_widget_get_next_sibling(child))
    if (has_label(child, label)) return true;
  return false;
}
static GtkWidget *find(GtkWidget *widget, const char *type, const char *label=nullptr) {
  if (!strcmp(G_OBJECT_TYPE_NAME(widget), type) && (!label || has_label(widget, label))) return widget;
  for (auto *child=gtk_widget_get_first_child(widget); child; child=gtk_widget_get_next_sibling(child))
    if (auto *result=find(child, type, label)) return result;
  return nullptr;
}
static void right_click(double x, double y) {
  graphene_point_t point{float(x),float(y)}, mapped;
  if (!gtk_widget_compute_point(host, GTK_WIDGET(window), &point, &mapped)) fail("coordinate transform");
  auto xid=std::to_string(gdk_x11_surface_get_xid(gtk_native_get_surface(GTK_NATIVE(window))));
  std::vector<std::string> args{g_getenv("GNOSIS_XDOTOOL"), "windowfocus", xid, "mousemove", "--window", xid,
                                std::to_string(int(mapped.x)), std::to_string(int(mapped.y)), "click", "3"};
  std::vector<char *> argv;
  for (auto &arg:args) argv.push_back(arg.data());
  argv.push_back(nullptr);
  int status=0;
  clicked=g_get_monotonic_time();
  if (!g_spawn_sync(nullptr, argv.data(), nullptr, G_SPAWN_DEFAULT, nullptr, nullptr, nullptr, nullptr, &status, nullptr) || status)
    fail("XTest right click");
}
static void read_image() {
  busy=true;
  gdk_clipboard_read_texture_async(gtk_widget_get_clipboard(GTK_WIDGET(window)), nullptr,
    +[](GObject *source, GAsyncResult *result, gpointer) {
      g_autoptr(GError) error=nullptr;
      g_autoptr(GdkTexture) texture=gdk_clipboard_read_texture_finish(GDK_CLIPBOARD(source), result, &error);
      if (!texture || error || gdk_texture_get_width(texture)!=64 || gdk_texture_get_height(texture)!=48)
        fail("clipboard did not preserve original image dimensions");
      std::vector<guchar> pixels(64*48*4);
      auto *download=gdk_texture_downloader_new(texture);
      gdk_texture_downloader_set_format(download, GDK_MEMORY_R8G8B8A8);
      gdk_texture_downloader_download_into(download, pixels.data(), 64*4);
      gdk_texture_downloader_free(download);
      for (size_t i=0; i<pixels.size(); i+=4)
        if (pixels[i]!=(scenario==2?0:255) || pixels[i+1]!=0 || pixels[i+2]!=(scenario==2?255:0) || pixels[i+3]!=255)
          fail("clipboard pixels do not match original resource");
      g_print("IMAGE_COPY_CASE_PASS %d original 64x48 pixels\n", scenario);
      busy=false; stage=3;
    }, nullptr);
}
static gboolean tick(gpointer) {
  auto now=g_get_monotonic_time();
  if (now-started>35*G_USEC_PER_SEC) fail("timeout");
  if (busy || page->session.empty() || page->loading || page->native_tab_id()<0) return G_SOURCE_CONTINUE;
  auto *dialog=adw_application_window_get_visible_dialog(ADW_APPLICATION_WINDOW(window));
  if (dialog && ADW_IS_ALERT_DIALOG(dialog)) { adw_dialog_force_close(dialog); return G_SOURCE_CONTINUE; }
  if (stage==0) {
    busy=true;
    page->command("Runtime.evaluate", "{\"expression\":\"(()=>{const i=document.images[0];if(!i||!i.complete||!i.naturalWidth)return null;const r=i.getBoundingClientRect();return{x:r.x+r.width/2,y:r.y+r.height/2}})()\",\"returnByValue\":true}",
      [](JsonObject *result, const std::string &error) {
        busy=false;
        if (!result || !error.empty() || json_object_has_member(result,"exceptionDetails")) fail("image geometry evaluation");
        auto *remote=json_object_get_object_member(result,"result");
        auto *node=json_object_get_member(remote,"value");
        if (!node || !JSON_NODE_HOLDS_OBJECT(node)) return;
        auto *point=json_node_get_object(node);
        right_click(json_object_get_double_member(point,"x"), json_object_get_double_member(point,"y"));
        stage=1;
      });
  } else if (stage==1) {
    auto *button=find(GTK_WIDGET(window),"GtkModelButton","Copy Image");
    if (button && gtk_widget_get_mapped(button) && gtk_widget_is_sensitive(button)) {
      double ms=(now-clicked)/1000.;
      g_print("IMAGE_COPY_READY_MS case=%d %.1f\n",scenario,ms);
      if (ms>400) fail("copy availability exceeded 400ms");
      gtk_widget_activate(button); stage=2; read_image();
    } else if (now-clicked>4*G_USEC_PER_SEC) fail("Copy Image remained disabled");
  } else if (stage==3) {
    if (++scenario<3) { stage=0; page->navigate(origin+(scenario==1?"/embedded-webp":"/embedded-svg")); }
    else {
      g_print("IMAGE_COPY_PASS direct WebP, scaled cross-origin WebP, SVG\n");
      page.reset();
      g_action_group_activate_action(G_ACTION_GROUP(g_application_get_default()),"quit",nullptr);
      return G_SOURCE_REMOVE;
    }
  }
  return G_SOURCE_CONTINUE;
}
static gboolean start(gpointer) {
  auto *app=g_application_get_default();
  if (!app) return G_SOURCE_CONTINUE;
  window=gtk_application_get_active_window(GTK_APPLICATION(app));
  if (!window) return G_SOURCE_CONTINUE;
  auto *embed=reinterpret_cast<void *(*)(void *)>(dlsym(RTLD_DEFAULT,"ephy_window_get_active_embed"))(window);
  if (!embed) return G_SOURCE_CONTINUE;
  auto *view=reinterpret_cast<GtkWidget *(*)(void *)>(dlsym(RTLD_DEFAULT,"ephy_embed_get_web_view"))(embed);
  host=find(view,"GnosisCompositorView");
  if (!host) return G_SOURCE_CONTINUE;
  page=reinterpret_cast<std::shared_ptr<GnosisBrave::Page>(*)(void *)>(dlsym(RTLD_DEFAULT,"_Z27ephy_engine_view_brave_pageP15_EphyEngineView"))(view);
  if (!page) return G_SOURCE_CONTINUE;
  origin=g_getenv("GNOSIS_IMAGE_COPY_ORIGIN");
  started=g_get_monotonic_time();
  g_timeout_add(20,tick,nullptr);
  return G_SOURCE_REMOVE;
}
__attribute__((constructor)) static void setup() {
  char exe[2048]; auto n=readlink("/proc/self/exe",exe,sizeof(exe)-1);
  if (n<0) return;
  exe[n]=0;
  if (strstr(exe,"/.gnosis-wrapped")) g_timeout_add_seconds(3,start,nullptr);
}
