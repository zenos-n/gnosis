/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "browser.hpp"
#include <adwaita.h>
#include <dlfcn.h>
#include <unistd.h>
#include <cstring>
static GtkWindow *window;
static AdwTabView *tabs;
static std::shared_ptr<GnosisBrave::Page> anchor, victim;
static std::string closed_target;
static int stage;
static bool busy;
static gint64 started, changed;
static void fail(const char *why) { g_printerr("TAB_LIFETIME_FAIL stage=%d %s\n",stage,why); _exit(77); }
static GtkWidget *find(GtkWidget *w,const char *type) {
  if(!strcmp(G_OBJECT_TYPE_NAME(w),type))return w;
  for(auto *c=gtk_widget_get_first_child(w);c;c=gtk_widget_get_next_sibling(c))if(auto *found=find(c,type))return found;
  return nullptr;
}
static auto active() {
  auto embed=reinterpret_cast<void *(*)(void *)>(dlsym(RTLD_DEFAULT,"ephy_window_get_active_embed"))(window);
  auto view=reinterpret_cast<void *(*)(void *)>(dlsym(RTLD_DEFAULT,"ephy_embed_get_web_view"))(embed);
  return reinterpret_cast<std::shared_ptr<GnosisBrave::Page>(*)(void *)>(dlsym(RTLD_DEFAULT,"_Z27ephy_engine_view_brave_pageP15_EphyEngineView"))(view);
}
static void targets(std::function<void(JsonArray *)> done) {
  busy=true;
  GnosisBrave::browser_command(false,"Target.getTargets","{}",[done](JsonObject *r,const std::string &error){
    busy=false;if(!r||!error.empty())fail("managed browser connection lost");done(json_object_get_array_member(r,"targetInfos"));
  });
}
static gboolean tick(gpointer) {
  auto now=g_get_monotonic_time();if(now-started>40*G_USEC_PER_SEC)fail("timeout");
  if(busy||now-changed<300000)return G_SOURCE_CONTINUE;
  auto *dialog=adw_application_window_get_visible_dialog(ADW_APPLICATION_WINDOW(window));
  if(dialog){adw_dialog_force_close(dialog);return G_SOURCE_CONTINUE;}
  if(stage==0) {
    if(anchor->loading||anchor->session.empty())return G_SOURCE_CONTINUE;
    gtk_widget_activate_action(GTK_WIDGET(window),"win.new-tab",nullptr);victim=active();
    victim->navigate(anchor->uri+"?closed-document");stage=1;
  } else if(stage==1) {
    if(victim->loading||victim->session.empty()||victim->uri.find("closed-document")==std::string::npos)return G_SOURCE_CONTINUE;
    closed_target=victim->target;busy=true;
    victim->command("Runtime.evaluate","{\"expression\":\"onbeforeunload=e=>{e.preventDefault();e.returnValue='';};setInterval(()=>document.title='still live '+Date.now(),50);true\"}",[](JsonObject *r,const std::string &error){
      busy=false;if(!r||!error.empty())fail("fixture setup");
      adw_tab_view_close_page(tabs,adw_tab_view_get_selected_page(tabs));stage=2;changed=g_get_monotonic_time();
    });
  } else if(stage==2) {
    if(!victim->is_closed()||adw_tab_view_get_n_pages(tabs)!=1)return G_SOURCE_CONTINUE;
    targets([](JsonArray *items){
      for(guint i=0;i<json_array_get_length(items);++i) {
        auto *item=json_array_get_object_element(items,i);
        if(closed_target==json_object_get_string_member_with_default(item,"targetId",""))fail("closed document remains a hidden target");
      }
      g_print("TAB_LIFETIME_DESTROY_PASS closed tab has no surviving WebContents\n");
      anchor->close();stage=3;changed=g_get_monotonic_time();
    });
  } else if(stage==3) {
    targets([](JsonArray *items){
      for(guint i=0;i<json_array_get_length(items);++i)if(!strcmp(json_object_get_string_member_with_default(json_array_get_object_element(items,i),"type",""),"page"))fail("zero-tab browser retained a page");
      gtk_widget_activate_action(GTK_WIDGET(window),"win.new-tab",nullptr);victim=active();victim->navigate("data:text/html,<title>Fresh managed tab</title>");stage=4;
    });
  } else if(stage==4) {
    if(victim->loading||victim->session.empty()||victim->title!="Fresh managed tab")return G_SOURCE_CONTINUE;
    g_print("TAB_LIFETIME_PASS zero live documents, managed engine reopens a fresh native tab\n");
    g_action_group_activate_action(G_ACTION_GROUP(g_application_get_default()),"quit",nullptr);return G_SOURCE_REMOVE;
  }
  return G_SOURCE_CONTINUE;
}
static gboolean start(gpointer) {
  window=gtk_application_get_active_window(GTK_APPLICATION(g_application_get_default()));if(!window)return G_SOURCE_CONTINUE;
  tabs=ADW_TAB_VIEW(find(GTK_WIDGET(window),"AdwTabView"));if(!tabs)return G_SOURCE_CONTINUE;
  anchor=active();started=changed=g_get_monotonic_time();g_timeout_add(50,tick,nullptr);return G_SOURCE_REMOVE;
}
__attribute__((constructor)) static void setup() {
  char exe[2048];auto n=readlink("/proc/self/exe",exe,sizeof exe-1);if(n<0)return;exe[n]=0;
  if(strstr(exe,"/.gnosis-wrapped"))g_timeout_add_seconds(2,start,nullptr);
}
