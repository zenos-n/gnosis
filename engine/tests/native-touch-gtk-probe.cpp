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
static bool monitor_selected;
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
static void capture_preview(const char *name,bool left_red,bool right_red) {
  auto *paintable=gtk_widget_paintable_new(host);auto *snapshot=gtk_snapshot_new();
  gdk_paintable_snapshot(paintable,GDK_SNAPSHOT(snapshot),width,height);
  auto *node=gtk_snapshot_free_to_node(snapshot);if(!node)fail("empty preview");
  auto *texture=gsk_renderer_render_texture(gtk_native_get_renderer(gtk_widget_get_native(host)),node,nullptr);
  std::string path=std::string("/tmp/gnosis-swipe-")+name+".png";
  gdk_texture_save_to_png(texture,path.c_str());
  auto *downloader=gdk_texture_downloader_new(texture);
  gdk_texture_downloader_set_format(downloader,GDK_MEMORY_R8G8B8A8_PREMULTIPLIED);
  gsize stride=0;auto *bytes=gdk_texture_downloader_download_bytes(downloader,&stride);
  auto *pixels=static_cast<const guchar*>(g_bytes_get_data(bytes,nullptr));
  auto color=[&](double x,bool red){auto *p=pixels+int(height*.7)*stride+int(width*x)*4;
    g_print("SWIPE_PIXEL %s x=%.2f rgb=%u,%u,%u\n",name,x,p[0],p[1],p[2]);
    return red ? p[0]>180&&p[2]<80 : p[2]>180&&p[0]<80;};
  bool valid=color(.1,left_red)&&color(.85,right_red);
  g_bytes_unref(bytes);gdk_texture_downloader_free(downloader);g_object_unref(texture);
  gsk_render_node_unref(node);g_object_unref(paintable);
  if(!valid)fail("swipe did not display the destination/current page colors");
}
static void prepare_preview_actions() {
  const std::string url=page->uri;
  add(1000,initialize_input);
  add(300,[]{touch("Down",0,width*.5,height*.7);});add(500,[]{touch("Up",0);});
  add(800,[url]{page->navigate(url+"?color=red");});
  add(50,[]{evaluate("new URL(location.href).searchParams.get('color')==='red'");});
  add(800,[url]{page->navigate(url+"?color=blue");});
  add(50,[]{evaluate("new URL(location.href).searchParams.get('color')==='blue'");});
  add(40,[]{touch("Down",0,width*.05,height*.7);});
  for(int i=1;i<=15;i++)add(20,[i]{touch("Motion",0,width*(.05+.35*i/15),height*.7);});
  add(50,[]{capture_preview("back-drag",true,false);});
  add(50,[]{touch("Motion",0,width*.05+20,height*.7);});
  add(600,[]{touch("Up",0);});
  add(50,[]{evaluate("new URL(location.href).searchParams.get('color')==='blue'");capture_preview("cancelled",false,false);});
  swipe(width*.05,height*.7,width*.35,0);
  add(50,[]{evaluate("new URL(location.href).searchParams.get('color')==='red'");capture_preview("back-complete",true,true);});
  add(40,[]{touch("Down",0,width*.95,height*.7);});
  for(int i=1;i<=15;i++)add(20,[i]{touch("Motion",0,width*(.95-.35*i/15),height*.7);});
  add(50,[]{capture_preview("forward-drag",true,false);});
  add(600,[]{touch("Up",0);});
  add(50,[]{evaluate("new URL(location.href).searchParams.get('color')==='blue'");capture_preview("forward-complete",false,false);});
  add(0,[]{stop_input();page.reset();g_print("NATIVE_SWIPE_PREVIEW_PASS actual destination pixels, cancelled drag, back/forward navigation\nNATIVE_TOUCH_PASS\n");g_action_group_activate_action(G_ACTION_GROUP(g_application_get_default()),"quit",nullptr);});
}
static void prepare_trackpad_actions() {
  auto *events=gtk_event_controller_legacy_new();
  gtk_event_controller_set_propagation_phase(events,GTK_PHASE_CAPTURE);
  g_signal_connect(events,"event",G_CALLBACK((+[](GtkEventControllerLegacy*,GdkEvent *event,gpointer)->gboolean{
    if(gdk_event_get_event_type(event)==GDK_SCROLL) {
      double dx,dy;gdk_scroll_event_get_deltas(event,&dx,&dy);
      g_print("SCROLL_GDK unit=%d dx=%.2f dy=%.2f stop=%d\n",gdk_scroll_event_get_unit(event),dx,dy,gdk_scroll_event_is_stop(event));
    }
    return FALSE;
  })),nullptr);
  gtk_widget_add_controller(GTK_WIDGET(window),events);
  add(1000,initialize_input);
  add(500,[]{
    graphene_rect_t bounds;if(!gtk_widget_compute_bounds(host,GTK_WIDGET(window),&bounds))fail("pointer bounds");
    g_variant_unref(call("org.gnome.Mutter.RemoteDesktop",remote,"org.gnome.Mutter.RemoteDesktop.Session","NotifyPointerMotionAbsolute",
      g_variant_new("(sdd)",stream.c_str(),bounds.origin.x+width*.5,bounds.origin.y+height*.7)));
  });
  add(100,[]{evaluate("scrollTo(0,0),true");});
  for(int i=0;i<15;i++)add(20,[]{g_variant_unref(call("org.gnome.Mutter.RemoteDesktop",remote,
    "org.gnome.Mutter.RemoteDesktop.Session","NotifyPointerAxis",g_variant_new("(ddu)",0.,20.,0u)));});
  add(20,[]{g_variant_unref(call("org.gnome.Mutter.RemoteDesktop",remote,
    "org.gnome.Mutter.RemoteDesktop.Session","NotifyPointerAxis",g_variant_new("(ddu)",0.,0.,5u)));});
  add(250,[]{evaluate("globalThis.releaseScrollY=scrollY,true");});
  add(50,[]{evaluate("releaseScrollY>100&&releaseScrollY<1000&&scrollY>releaseScrollY+20");});
  add(0,[]{stop_input();page.reset();g_print("NATIVE_TRACKPAD_PASS surface deltas, axis stop and momentum\nNATIVE_TOUCH_PASS\n");g_action_group_activate_action(G_ACTION_GROUP(g_application_get_default()),"quit",nullptr);});
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
  add(35,[]{touch("Down",0,width*.5,height*.8);});
  for(int i=1;i<=15;i++)add(20,[i]{touch("Motion",0,width*.5,height*.8-250.*i/15);});
  add(20,[]{touch("Up",0);});
  add(250,[]{evaluate("globalThis.releaseScrollY=scrollY,true");});
  add(50,[]{evaluate("scrollY>releaseScrollY+30");});
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
  if(!monitor_selected) {
    auto *monitors=gdk_display_get_monitors(gdk_display_get_default());
    for(guint i=0;i<g_list_model_get_n_items(monitors);i++) {
      auto *monitor=GDK_MONITOR(g_list_model_get_item(monitors,i));
      const char *name=gdk_monitor_get_connector(monitor);
      if(name&&strstr(name,"HDMI")){gtk_window_fullscreen_on_monitor(window,monitor);monitor_selected=true;}
      g_object_unref(monitor);if(monitor_selected)return G_SOURCE_CONTINUE;
    }
    fail("HDMI test monitor not available");
  }
  if(!gtk_window_is_fullscreen(window))return G_SOURCE_CONTINUE;
  width=gtk_widget_get_width(host);height=gtk_widget_get_height(host);
  if(width<600||height<500)return G_SOURCE_CONTINUE;
  started=g_get_monotonic_time();next_at=started+1000000;
  if(g_getenv("GNOSIS_TOUCH_PREVIEW_TEST"))prepare_preview_actions();
  else if(g_getenv("GNOSIS_TRACKPAD_TEST"))prepare_trackpad_actions();
  else prepare_actions();
  g_timeout_add(15,tick,nullptr);return G_SOURCE_REMOVE;
}
__attribute__((constructor))static void setup(){char exe[2048];auto n=readlink("/proc/self/exe",exe,sizeof(exe)-1);if(n<0)return;exe[n]=0;if(strstr(exe,"/.gnosis-wrapped"))g_timeout_add_seconds(2,start,nullptr);}
