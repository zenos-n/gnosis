/* SPDX-License-Identifier: GPL-3.0-or-later
 * Disposable-profile test: real Mutter touchscreen -> GTK -> wl_touch -> Brave.
 * Private DevTools only observes the local fixture; it does not inject touches.
 */
#include "browser.hpp"
#include <adwaita.h>
#include <dlfcn.h>
#include <unistd.h>
#include <cstring>
#include <vector>

static std::shared_ptr<GnosisBrave::Page> page;
static GtkWidget *host;
static GtkWindow *window;
static GDBusConnection *bus;
static std::string remote, stream;
static double width, height;
static bool busy;
static gint64 started, next_at;
static size_t step;
static std::vector<std::pair<int,std::function<void()>>> actions;

static void stop_input() {
  if (!bus || remote.empty()) return;
  auto *r=g_dbus_connection_call_sync(bus,"org.gnome.Mutter.RemoteDesktop",remote.c_str(),
    "org.gnome.Mutter.RemoteDesktop.Session","Stop",nullptr,nullptr,G_DBUS_CALL_FLAGS_NONE,3000,nullptr,nullptr);
  if(r)g_variant_unref(r); remote.clear();
}
static void fail(const char *message) {
  g_printerr("TOUCH_FAIL step=%zu %s\n",step,message);stop_input();_exit(77);
}
static GVariant *call(const char *dest,const std::string& path,const char *iface,const char *method,GVariant *args=nullptr) {
  GError *error=nullptr;
  auto *r=g_dbus_connection_call_sync(bus,dest,path.c_str(),iface,method,args,nullptr,G_DBUS_CALL_FLAGS_NONE,5000,nullptr,&error);
  if(!r)fail(error?error->message:"D-Bus call failed");return r;
}
static std::string path_result(GVariant *r) {
  const char *path;g_variant_get(r,"(&o)",&path);std::string result=path;g_variant_unref(r);return result;
}
static void initialize_input() {
  bus=g_bus_get_sync(G_BUS_TYPE_SESSION,nullptr,nullptr);if(!bus)fail("no session bus");
  remote=path_result(call("org.gnome.Mutter.RemoteDesktop","/org/gnome/Mutter/RemoteDesktop",
    "org.gnome.Mutter.RemoteDesktop","CreateSession"));
  auto *r=call("org.gnome.Mutter.RemoteDesktop",remote,"org.freedesktop.DBus.Properties","Get",
    g_variant_new("(ss)","org.gnome.Mutter.RemoteDesktop.Session","SessionId"));
  GVariant *id;g_variant_get(r,"(v)",&id);
  GVariantBuilder props;g_variant_builder_init(&props,G_VARIANT_TYPE_VARDICT);
  g_variant_builder_add(&props,"{sv}","remote-desktop-session-id",id);
  auto cast=path_result(call("org.gnome.Mutter.ScreenCast","/org/gnome/Mutter/ScreenCast",
    "org.gnome.Mutter.ScreenCast","CreateSession",g_variant_new("(a{sv})",&props)));
  g_variant_unref(id);g_variant_unref(r);
  auto *monitor=gdk_display_get_monitor_at_surface(gdk_display_get_default(),gtk_native_get_surface(GTK_NATIVE(window)));
  const char *connector=monitor?gdk_monitor_get_connector(monitor):nullptr;if(!connector)fail("monitor has no connector");
  g_variant_builder_init(&props,G_VARIANT_TYPE_VARDICT);
  g_variant_builder_add(&props,"{sv}","cursor-mode",g_variant_new_uint32(0));
  stream=path_result(call("org.gnome.Mutter.ScreenCast",cast,"org.gnome.Mutter.ScreenCast.Session",
    "RecordMonitor",g_variant_new("(sa{sv})",connector,&props)));
  g_variant_unref(call("org.gnome.Mutter.RemoteDesktop",remote,"org.gnome.Mutter.RemoteDesktop.Session","Start"));
  g_print("TOUCH_INPUT monitor=%s host=%.0fx%.0f\n",connector,width,height);
}
static void touch(const char *phase,unsigned slot,double x=0,double y=0) {
  graphene_rect_t bounds;
  if(!gtk_widget_compute_bounds(host,GTK_WIDGET(window),&bounds))fail("host bounds");
  std::string method=std::string("NotifyTouch")+phase;
  auto *args=!strcmp(phase,"Up")?g_variant_new("(u)",slot):
    g_variant_new("(sudd)",stream.c_str(),slot,x+bounds.origin.x,y+bounds.origin.y);
  g_variant_unref(call("org.gnome.Mutter.RemoteDesktop",remote,"org.gnome.Mutter.RemoteDesktop.Session",method.c_str(),args));
}
static void evaluate(const std::string& expression) {
  busy=true;
  page->command("Runtime.evaluate","{\"expression\":"+GnosisBrave::quote(expression)+",\"returnByValue\":true}",
    [expression](JsonObject *r,const std::string& error){
      busy=false;auto *v=r?json_object_get_object_member(r,"result"):nullptr;
      if(!error.empty()||!v||json_object_has_member(r,"exceptionDetails")||!json_object_get_boolean_member_with_default(v,"value",false)) {
        if(v){auto *n=json_node_new(JSON_NODE_OBJECT);json_node_set_object(n,v);auto *s=json_to_string(n,false);g_printerr("TOUCH_RESULT %s\n",s);g_free(s);json_node_free(n);}
        page->command("Runtime.evaluate",R"JSON({"expression":"JSON.stringify({stats,scale:visualViewport.scale,scroll:scrollY,url:location.href})","returnByValue":true})JSON",
          [expression](JsonObject *state,const std::string&){
            auto *result=state?json_object_get_object_member(state,"result"):nullptr;
            if(result)g_printerr("TOUCH_STATE %s\n",json_object_get_string_member_with_default(result,"value","unknown"));
            fail(expression.c_str());
          });
        return;
      }
      g_print("TOUCH_CHECK %s\n",expression.c_str());
    });
}
static void setting(bool enabled) {
  auto get=reinterpret_cast<GSettings*(*)(const char*)>(dlsym(RTLD_DEFAULT,"ephy_settings_get"));
  if(!get||!g_settings_set_boolean(get("com.neg-zero.Gnosis.web"),"enable-navigation-gestures",enabled))fail("gesture preference");
}
static void add(int wait,std::function<void()> action){actions.emplace_back(wait,std::move(action));}
static void swipe(double x,double y,double dx,double dy) {
  add(35,[=]{touch("Down",0,x,y);});
  for(int i=1;i<=15;i++)add(25,[=]{touch("Motion",0,x+dx*i/15,y+dy*i/15);});
  add(500,[]{touch("Up",0);});
}
static void prepare_actions() {
  add(1000,initialize_input);
  // Mutter may advertise its virtual touchscreen only after the first contact.
  add(300,[]{touch("Down",0,width*.5,height*.7);});add(500,[]{touch("Up",0);});
  add(100,[]{evaluate("Object.assign(stats,{clicks:0,max:0,starts:0,moves:0,ends:0,cancels:0,trusted:true}),scrollTo(0,0),true");});
  add(50,[]{touch("Down",0,100,70);});add(450,[]{touch("Up",0);});
  add(50,[]{evaluate("stats.clicks===1&&stats.pointer==='touch'&&stats.trusted&&navigator.maxTouchPoints>=2");});
  add(100,[]{for(unsigned i=0;i<6;i++)touch("Down",i,width*.4+i*12,200);});
  add(100,[]{for(unsigned i=0;i<6;i++)touch("Motion",i,width*.4+i*12+8,220);});
  add(300,[]{for(unsigned i=0;i<6;i++)touch("Up",i);});
  add(50,[]{evaluate("stats.max===6&&stats.moves>0&&stats.ends>=7&&stats.trusted");});
  swipe(width*.5,height*.8,0,-250);
  add(50,[]{evaluate("scrollY>80");});
  add(300,[]{evaluate("scrollTo(0,0),true");});
  add(80,[]{touch("Down",0,width*.5-60,height*.6);touch("Down",1,width*.5+60,height*.6);});
  for(int i=1;i<=20;i++)add(30,[i]{touch("Motion",0,width*.5-60-i*6,height*.6);touch("Motion",1,width*.5+60+i*6,height*.6);});
  add(500,[]{touch("Up",0);touch("Up",1);});
  add(50,[]{evaluate("visualViewport.scale>1.15");});
  add(80,[]{touch("Down",0,width*.5-180,height*.6);touch("Down",1,width*.5+180,height*.6);});
  for(int i=1;i<=20;i++)add(30,[i]{touch("Motion",0,width*.5-180+i*8,height*.6);touch("Motion",1,width*.5+180-i*8,height*.6);});
  add(500,[]{touch("Up",0);touch("Up",1);});
  add(50,[]{evaluate("visualViewport.scale<1.05");});
  add(500,[]{setting(true);evaluate("history.pushState({},'', '?one'),history.pushState({},'', '?two'),true");});
  swipe(width*.05,height*.6,200,0);
  add(50,[]{evaluate("location.search==='?one'");});
  swipe(width*.95,height*.6,-200,0);
  add(50,[]{evaluate("location.search==='?two'");});
  add(150,[]{setting(false);});
  swipe(width*.05,height*.6,200,0);
  add(50,[]{evaluate("location.search==='?two'");});
  add(150,[]{setting(true);});
  swipe(width*.05,height*.8,0,-200);
  add(50,[]{evaluate("location.search==='?two'");});
  add(100,[]{touch("Down",0,width*.05,height*.6);touch("Down",1,width*.1,height*.6);});
  for(int i=1;i<=15;i++)add(25,[i]{touch("Motion",0,width*.05+i*14,height*.6);touch("Motion",1,width*.1+i*14,height*.6);});
  add(500,[]{touch("Up",0);touch("Up",1);});
  add(50,[]{evaluate("location.search==='?two'");});
  add(300,[]{evaluate("scrollTo(0,0),true");});
  add(150,[]{touch("Down",0,width*.5,210);});
  add(200,[]{gtk_widget_set_visible(host,FALSE);});
  add(300,[]{gtk_widget_set_visible(host,TRUE);});
  add(100,[]{touch("Up",0);});
  add(50,[]{touch("Down",0,100,70);});add(400,[]{touch("Up",0);});
  add(50,[]{evaluate("stats.clicks===2&&stats.cancels>0&&stats.trusted");});
  add(0,[]{stop_input();page.reset();g_print("NATIVE_TOUCH_PASS tap, six contacts, scroll, pinch, edge history, live toggle, cancellation\n");g_action_group_activate_action(G_ACTION_GROUP(g_application_get_default()),"quit",nullptr);});
}
static gboolean tick(gpointer) {
  auto now=g_get_monotonic_time();if(now-started>100*G_USEC_PER_SEC)fail("timeout");
  if(busy||now<next_at||step>=actions.size())return step>=actions.size()?G_SOURCE_REMOVE:G_SOURCE_CONTINUE;
  auto action=actions[step++];next_at=now+action.first*1000;action.second();return G_SOURCE_CONTINUE;
}
static GtkWidget *find(GtkWidget *w) {
  if(!strcmp(G_OBJECT_TYPE_NAME(w),"GnosisCompositorView"))return w;
  for(auto *c=gtk_widget_get_first_child(w);c;c=gtk_widget_get_next_sibling(c))if(auto *r=find(c))return r;
  return nullptr;
}
static gboolean start(gpointer) {
  auto *app=g_application_get_default();if(!app)return G_SOURCE_CONTINUE;
  window=gtk_application_get_active_window(GTK_APPLICATION(app));if(!window)return G_SOURCE_CONTINUE;
  auto *dialog=adw_application_window_get_visible_dialog(ADW_APPLICATION_WINDOW(window));
  if(dialog&&ADW_IS_ALERT_DIALOG(dialog)){g_signal_emit_by_name(dialog,"response","no");adw_dialog_close(dialog);return G_SOURCE_CONTINUE;}
  auto embed=reinterpret_cast<void*(*)(void*)>(dlsym(RTLD_DEFAULT,"ephy_window_get_active_embed"));
  auto getview=reinterpret_cast<GtkWidget*(*)(void*)>(dlsym(RTLD_DEFAULT,"ephy_embed_get_web_view"));
  auto getpage=reinterpret_cast<std::shared_ptr<GnosisBrave::Page>(*)(void*)>(dlsym(RTLD_DEFAULT,"_Z27ephy_engine_view_brave_pageP15_EphyEngineView"));
  if(!embed||!getview||!getpage||!embed(window))return G_SOURCE_CONTINUE;
  auto *view=getview(embed(window));host=find(view);page=getpage(view);
  if(!host||!page||page->loading||page->uri.find("native-touch.html")==std::string::npos)return G_SOURCE_CONTINUE;
  if(!gtk_window_is_fullscreen(window)){gtk_window_fullscreen(window);return G_SOURCE_CONTINUE;}
  width=gtk_widget_get_width(host);height=gtk_widget_get_height(host);
  if(width<600||height<500)return G_SOURCE_CONTINUE;
  started=g_get_monotonic_time();next_at=started+1000000;prepare_actions();g_timeout_add(15,tick,nullptr);return G_SOURCE_REMOVE;
}
__attribute__((constructor))static void setup(){char exe[2048];auto n=readlink("/proc/self/exe",exe,sizeof(exe)-1);if(n<0)return;exe[n]=0;if(strstr(exe,"/.gnosis-wrapped"))g_timeout_add_seconds(2,start,nullptr);}
