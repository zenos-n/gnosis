/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "browser.hpp"
#include <adwaita.h>
#include <dlfcn.h>
#include <unistd.h>
#include <fstream>
#include <cstring>
static GtkWindow *window;
static std::shared_ptr<GnosisBrave::Page> page;
static int stage;
static bool busy;
static bool navigated;
static gint64 started, changed;
static void fail(const char *message) { g_printerr("EXTERNAL_GTK_FAIL stage=%d %s\n",stage,message); _exit(77); }
static size_t calls() { std::ifstream input(g_getenv("GNOSIS_EXTERNAL_LOG")); std::string line; size_t count=0; while(std::getline(input,line)) { if(line!="gnosis-external-test://open/path?value=a%20b") fail("handler received different URI"); ++count; } return count; }
static void click() {
  busy=true;
  page->command("Runtime.evaluate","{\"expression\":\"document.querySelector('a').click()\",\"userGesture\":true}",[](JsonObject*,const std::string&error){busy=false;if(!error.empty())fail(error.c_str());});
  changed=g_get_monotonic_time();
}
static void navigate(const std::string&url) { page->navigate(url);changed=g_get_monotonic_time(); }
static bool has_label(GtkWidget*w,const char*t) { if(GTK_IS_LABEL(w)&&!g_strcmp0(gtk_label_get_text(GTK_LABEL(w)),t))return true;for(auto*c=gtk_widget_get_first_child(w);c;c=gtk_widget_get_next_sibling(c))if(has_label(c,t))return true;return false; }
static GtkWidget *button(GtkWidget*w,const char*t) { if(GTK_IS_BUTTON(w)&&has_label(w,t))return w;for(auto*c=gtk_widget_get_first_child(w);c;c=gtk_widget_get_next_sibling(c))if(auto*b=button(c,t))return b;return nullptr; }
static AdwAlertDialog *dialog() {
  auto *d=adw_application_window_get_visible_dialog(ADW_APPLICATION_WINDOW(window));
  return d&&ADW_IS_ALERT_DIALOG(d)?ADW_ALERT_DIALOG(d):nullptr;
}
static void capture(GtkWidget*w) {
  auto*p=gtk_widget_paintable_new(w);auto*s=gtk_snapshot_new();
  gdk_paintable_snapshot(p,GDK_SNAPSHOT(s),gtk_widget_get_width(w),gtk_widget_get_height(w));
  auto*n=gtk_snapshot_free_to_node(s);
  if(n){auto*t=gsk_renderer_render_texture(gtk_native_get_renderer(gtk_widget_get_native(w)),n,nullptr);gdk_texture_save_to_png(t,"/tmp/gnosis-external-dialog.png");g_object_unref(t);gsk_render_node_unref(n);}
  g_object_unref(p);
}
static void respond(AdwAlertDialog*d,bool accept,bool remember=false) {
  const auto *heading=adw_alert_dialog_get_heading(d);
  if(!strstr(heading,"Gnosis Test Handler")) fail("wrong native heading");
  if(strcmp(adw_alert_dialog_get_default_response(d),"cancel"))fail("Open is default response");
  auto *check=static_cast<GtkCheckButton*>(g_object_get_data(G_OBJECT(d),"external-remember"));
  if(!check||gtk_check_button_get_active(check))fail("remember checkbox missing or preselected");
  if(!adw_alert_dialog_get_response_enabled(d,"accept"))return;
  if(stage==1)capture(GTK_WIDGET(d));
  if(remember)gtk_check_button_set_active(check,true);
  g_signal_emit_by_name(d,"response",accept?"accept":"cancel");
  ++stage;changed=g_get_monotonic_time();
}
static gboolean tick(gpointer) {
  if(g_get_monotonic_time()-started>55*G_USEC_PER_SEC)fail("timeout");
  if(busy||page->session.empty())return G_SOURCE_CONTINUE;
  auto*d=dialog();auto elapsed=g_get_monotonic_time()-changed;
  if(d&&strstr(adw_alert_dialog_get_heading(d),"Default Browser")){if(auto*b=button(GTK_WIDGET(d),"No"))gtk_widget_activate(b);return G_SOURCE_CONTINUE;}
  switch(stage) {
  case 0:
    if(!navigated){g_print("EXTERNAL_TEST navigating fixture\n");navigated=true;page->navigate(g_getenv("GNOSIS_EXTERNAL_FIXTURE"));}
    busy=true;page->command("Runtime.evaluate","{\"expression\":\"document.readyState==='complete' && !!document.querySelector('a')\",\"returnByValue\":true}",[](JsonObject*r,const std::string&){busy=false;auto*v=r?json_object_get_object_member(r,"result"):nullptr;if(v&&json_object_get_boolean_member_with_default(v,"value",false)){stage=1;click();}});break;
  case 1:if(d)respond(d,false);break;
  case 2:if(elapsed>600000){if(g_getenv("GNOSIS_EXTERNAL_PRIVATE_TEST")){if(calls()!=3)fail("private Cancel launched app");g_print("EXTERNAL_PRIVATE_PASS normal remembered grant did not bypass private confirmation\n");page.reset();g_action_group_activate_action(G_ACTION_GROUP(g_application_get_default()),"quit",nullptr);return G_SOURCE_REMOVE;}if(calls())fail("Cancel launched app");stage=3;click();}break;
  case 3:if(d)respond(d,true);break;
  case 4:if(calls()==1){stage=5;click();}break;
  case 5:if(d)respond(d,true,true);break;
  case 6:if(calls()==2){stage=7;click();}break;
  case 7:if(d)fail("remembered origin still prompts");if(calls()==3){stage=8;std::string url=g_getenv("GNOSIS_EXTERNAL_FIXTURE");url.replace(url.find("127.0.0.1"),9,"localhost");navigate(url);}break;
  case 8:if(elapsed>800000){stage=9;click();}break;
  case 9:if(d)respond(d,false);break;
  case 10:if(elapsed>600000){if(calls()!=3)fail("grant leaked across origins");stage=11;click();}break;
  case 11:if(d){page->command("Runtime.evaluate","{\"expression\":\"location.href='about:blank'\"}");changed=g_get_monotonic_time();stage=12;}break;
  case 12:if(elapsed>800000){if(dialog())fail("navigation left dialog alive");if(calls()!=3)fail("navigation launched app");g_print("EXTERNAL_GTK_PASS cancel, open-once, origin-scoped remember, navigation cleanup, exact URI\n");page.reset();g_action_group_activate_action(G_ACTION_GROUP(g_application_get_default()),"quit",nullptr);return G_SOURCE_REMOVE;}break;
  }
  return G_SOURCE_CONTINUE;
}
static gboolean start(gpointer) {
  window=gtk_application_get_active_window(GTK_APPLICATION(g_application_get_default()));if(!window)return G_SOURCE_CONTINUE;
  auto e=reinterpret_cast<void*(*)(void*)>(dlsym(RTLD_DEFAULT,"ephy_window_get_active_embed"));
  auto v=reinterpret_cast<void*(*)(void*)>(dlsym(RTLD_DEFAULT,"ephy_embed_get_web_view"));
  auto p=reinterpret_cast<std::shared_ptr<GnosisBrave::Page>(*)(void*)>(dlsym(RTLD_DEFAULT,"_Z27ephy_engine_view_brave_pageP15_EphyEngineView"));
  if(!e(window))return G_SOURCE_CONTINUE;page=p(v(e(window)));started=changed=g_get_monotonic_time();g_timeout_add(100,tick,nullptr);return G_SOURCE_REMOVE;
}
__attribute__((constructor))static void setup(){char exe[2048];auto n=readlink("/proc/self/exe",exe,sizeof(exe)-1);if(n<0)return;exe[n]=0;if(strstr(exe,"/.gnosis-wrapped")){g_timeout_add_seconds(3,start,nullptr);}}
