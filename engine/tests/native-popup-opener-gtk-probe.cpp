/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "browser.hpp"
#include "compositor.h"
#include <adwaita.h>
#include <gdk/x11/gdkx.h>
#include <dlfcn.h>
#include <unistd.h>
#include <cstring>
#include <vector>
static GtkWindow *window;
static GtkWidget *host, *tabs;
static std::shared_ptr<GnosisBrave::Page> source, child;
static gint64 started, changed, clicked;
static int stage=-3, scenario, initial_tabs;
static int orphan_window=-1;
static std::string orphan, orphan_two;
static bool isolated;
static bool busy, post_started;
static void fail(const char* message) { g_printerr("POPUP_OPENER_FAIL stage=%d scenario=%d %s\n",stage,scenario,message);_exit(77); }
static GtkWidget* find(GtkWidget* w) {
 if(!strcmp(G_OBJECT_TYPE_NAME(w),"GnosisCompositorView"))return w;
 for(auto*c=gtk_widget_get_first_child(w);c;c=gtk_widget_get_next_sibling(c))if(auto*r=find(c))return r;
 return nullptr;
}
static int tab_count() { return reinterpret_cast<int(*)(void*)>(dlsym(RTLD_DEFAULT,"ephy_tab_view_get_n_pages"))(tabs); }
static void evaluate(std::shared_ptr<GnosisBrave::Page> page,const std::string& js,bool check=false) {
 busy=true;page->command("Runtime.evaluate","{\"expression\":"+GnosisBrave::quote(js)+",\"returnByValue\":true}",[check,page](JsonObject*r,const std::string&e){
  busy=false;
  if(!r||!e.empty()||json_object_has_member(r,"exceptionDetails"))fail("evaluation failed");
  if(check&&!json_object_get_boolean_member_with_default(json_object_get_object_member(r,"result"),"value",false)){busy=true;page->command("Runtime.evaluate","{\"expression\":\"JSON.stringify({gap:outerHeight-innerHeight,outer:outerHeight,inner:innerHeight,opener:!!opener,title:document.title})\",\"returnByValue\":true}",[](JsonObject *details,const std::string &){g_autoptr(JsonNode) node=json_node_new(JSON_NODE_OBJECT);json_node_set_object(node,details);g_autofree char *json=json_to_string(node,false);g_printerr("POPUP_GEOMETRY %s\n",json);fail("popup context assertion failed");});}
 });
}
static void click(bool press) {
 graphene_point_t p{40,120},mapped;
 if(!gtk_widget_compute_point(host,GTK_WIDGET(window),&p,&mapped))fail("input coordinate transform");
 auto id=std::to_string(gdk_x11_surface_get_xid(gtk_native_get_surface(GTK_NATIVE(window))));
 std::vector<std::string> args{g_getenv("GNOSIS_XDOTOOL"),"windowfocus",id,"mousemove","--window",id,std::to_string(int(mapped.x)),std::to_string(int(mapped.y)),press?"mousedown":"mouseup","1"};
 std::vector<char*> argv;for(auto&a:args)argv.push_back(a.data());argv.push_back(nullptr);int status=0;
 if(!g_spawn_sync(nullptr,argv.data(),nullptr,G_SPAWN_DEFAULT,nullptr,nullptr,nullptr,nullptr,&status,nullptr)||status)fail("XTest input failed");
}
static bool capture(GtkWidget* view) {
 if(!gtk_widget_get_mapped(view)||gtk_widget_get_width(view)<=0||gtk_widget_get_height(view)<=0)return false;
 auto*p=gtk_widget_paintable_new(view);auto*s=gtk_snapshot_new();
 gdk_paintable_snapshot(p,GDK_SNAPSHOT(s),gtk_widget_get_width(view),gtk_widget_get_height(view));
 if(auto*n=gtk_snapshot_free_to_node(s)) {
  auto*t=gsk_renderer_render_texture(gtk_native_get_renderer(gtk_widget_get_native(view)),n,nullptr);
  if(!t)fail("native popup did not paint");
  auto *download=gdk_texture_downloader_new(t);gdk_texture_downloader_set_format(download,GDK_MEMORY_R8G8B8A8);
  int width=gdk_texture_get_width(t),height=gdk_texture_get_height(t);std::vector<guchar> pixels(size_t(width)*height*4);gdk_texture_downloader_download_into(download,pixels.data(),width*4);gdk_texture_downloader_free(download);
  size_t offset=(size_t(10)*width+width/2)*4;bool content=width>20&&height>20&&pixels[offset]==0x22&&pixels[offset+1]==0x44&&pixels[offset+2]==0x66;
  if(!content){g_object_unref(t);gsk_render_node_unref(n);g_object_unref(p);return false;}
  gdk_texture_save_to_png(t,g_getenv("GNOSIS_POPUP_CAPTURE")?g_getenv("GNOSIS_POPUP_CAPTURE"):"/tmp/gnosis-popup-opener.png");g_object_unref(t);gsk_render_node_unref(n);
 } else {g_object_unref(p);return false;}
 g_object_unref(p);return true;
}
static gboolean tick(gpointer) {
 auto now=g_get_monotonic_time();if(now-started>45*G_USEC_PER_SEC) {
  if(child)g_printerr("POPUP_STATE loading=%d tab=%d uri=%s\n",child->loading,child->native_tab_id(),child->uri.c_str());
  fail("timeout");
 }
 if(busy||source->session.empty()||source->loading||now-changed<100000)return G_SOURCE_CONTINUE;
 auto*d=adw_application_window_get_visible_dialog(ADW_APPLICATION_WINDOW(window));
 if(d&&ADW_IS_ALERT_DIALOG(d)){adw_dialog_force_close(d);return G_SOURCE_CONTINUE;}
 if(stage==-3||stage==-2){busy=true;const bool first=stage==-3;GnosisBrave::browser_command(g_getenv("GNOSIS_POPUP_PRIVATE")!=nullptr,"Target.createTarget","{\"url\":"+GnosisBrave::quote(source->uri+"?orphan="+std::to_string(stage))+",\"newWindow\":"+(first?"true":"false")+"}",[first](JsonObject *r,const std::string &e){busy=false;if(!r||!e.empty())fail("seed native browser tabs");if(first)orphan=json_object_get_string_member(r,"targetId");else orphan_two=json_object_get_string_member(r,"targetId");});}
 else if(stage==-1){busy=true;GnosisBrave::browser_command(g_getenv("GNOSIS_POPUP_PRIVATE")!=nullptr,"Browser.getWindowForTarget","{\"targetId\":"+GnosisBrave::quote(orphan)+"}",[](JsonObject *r,const std::string &e){if(!r||!e.empty())fail("seed browser window");orphan_window=json_object_get_int_member(r,"windowId");GnosisBrave::browser_command(g_getenv("GNOSIS_POPUP_PRIVATE")!=nullptr,"Browser.getWindowForTarget","{\"targetId\":"+GnosisBrave::quote(orphan_two)+"}",[](JsonObject *r,const std::string &e){busy=false;if(!r||!e.empty()||json_object_get_int_member(r,"windowId")!=orphan_window)fail("seed did not create a multi-tab native window");});});}
 else if(stage==0) {
  if(source->native_tab_id()<0)return G_SOURCE_CONTINUE;
  std::string action=scenario?"fixtureChild=window.open('about:blank','fixture-oauth');fixtureChild.retainedValue='original';fixtureChild.document.write('<title>Written popup</title>');fixtureChild.document.close();const f=document.createElement('form');f.method='POST';f.action='/post';f.target='fixture-oauth';f.innerHTML='<input name=token value=fixture-post>';document.body.append(f);window.fixtureForm=f"+std::string(scenario==2?";f.submit()":""):"fixtureChild=window.open('/callback','fixture-oauth','popup,width=500,height=400')";
  evaluate(source,"window.fixtureReceived=false;sessionStorage.setItem('oauth-fixture','expected');window.fixtureOrigin="+std::string(scenario?"location.origin.replace('127.0.0.1','localhost')":"location.origin")+";window.onmessage=e=>{if(e.source===fixtureChild&&e.origin===fixtureOrigin&&e.data==='fixture-ok')fixtureReceived=true};document.body.innerHTML='<button style=\"position:absolute;left:20px;top:100px;width:200px;height:40px\">Sign in</button>';document.querySelector('button').onclick=()=>{"+action+"}");
 } else if(stage==1)click(true);
 else if(stage==2){clicked=now;click(false);}
 else if(stage==3) {
  if(!child||child->loading||child->session.empty()||child->native_tab_id()<0)return G_SOURCE_CONTINUE;
  auto*v=find(child->widget());
  if(!v||!gnosis_compositor_view_has_surface(GNOSIS_COMPOSITOR_VIEW(v)))return G_SOURCE_CONTINUE;
  if(!isolated){busy=true;GnosisBrave::browser_command(g_getenv("GNOSIS_POPUP_PRIVATE")!=nullptr,"Browser.getWindowForTarget","{\"targetId\":"+GnosisBrave::quote(child->target)+"}",[](JsonObject *r,const std::string &e){busy=false;if(!r||!e.empty()||json_object_get_int_member(r,"windowId")==orphan_window)fail("adopted entire multi-tab Brave window");isolated=true;});return G_SOURCE_CONTINUE;}
  if(scenario==1&&!post_started) {
   evaluate(child,"!!opener && retainedValue==='original' && document.title==='Written popup' && sessionStorage.getItem('oauth-fixture')==='expected'",true);
   post_started=true;stage=40;changed=now;return G_SOURCE_CONTINUE;
  }
  if(!child->uri.ends_with("/callback"))return G_SOURCE_CONTINUE;
  if(tab_count()!=initial_tabs+1)fail("duplicate popup GTK tabs");
  if(!capture(v))return G_SOURCE_CONTINUE;
  evaluate(child,"!!opener && document.title==='Callback fixture' && "+std::string(scenario?"location.hostname==='localhost'":"sessionStorage.getItem('oauth-fixture')==='expected'"),true);
 } else if(stage==40) {evaluate(source,"fixtureForm.submit()");stage=3;changed=now;return G_SOURCE_CONTINUE;}
 else if(stage==4)evaluate(source,"!!fixtureChild && !fixtureChild.closed",true);
 else if(stage==5)evaluate(child,"opener.postMessage('fixture-ok',location.origin.replace('localhost','127.0.0.1'))");
 else if(stage==6)evaluate(source,"fixtureReceived",true);
 else if(stage==7)child->command("Runtime.evaluate","{\"expression\":\"window.close()\"}");
 else if(stage==8) {
  if(!child->is_closed()||tab_count()!=initial_tabs)return G_SOURCE_CONTINUE;
  evaluate(source,"fixtureChild.closed",true);
 } else if(scenario<2) {child.reset();isolated=false;++scenario;stage=0;changed=now;return G_SOURCE_CONTINUE;}
 else {source.reset();child.reset();g_print("POPUP_OPENER_PASS original context, storage, cross-origin POST redirect, postMessage, native surface and script close\n");g_action_group_activate_action(G_ACTION_GROUP(g_application_get_default()),"quit",nullptr);return G_SOURCE_REMOVE;}
 ++stage;changed=now;return G_SOURCE_CONTINUE;
}
static gboolean start(gpointer) {
 auto*a=g_application_get_default();if(!a)return G_SOURCE_CONTINUE;
 window=gtk_application_get_active_window(GTK_APPLICATION(a));if(!window)return G_SOURCE_CONTINUE;
 auto embed=reinterpret_cast<void*(*)(void*)>(dlsym(RTLD_DEFAULT,"ephy_window_get_active_embed"))(window);if(!embed)return G_SOURCE_CONTINUE;
 auto view=reinterpret_cast<GtkWidget*(*)(void*)>(dlsym(RTLD_DEFAULT,"ephy_embed_get_web_view"))(embed);host=find(view);if(!host)return G_SOURCE_CONTINUE;
 source=reinterpret_cast<std::shared_ptr<GnosisBrave::Page>(*)(void*)>(dlsym(RTLD_DEFAULT,"_Z27ephy_engine_view_brave_pageP15_EphyEngineView"))(view);if(!source)return G_SOURCE_CONTINUE;
 tabs=reinterpret_cast<GtkWidget*(*)(void*)>(dlsym(RTLD_DEFAULT,"ephy_window_get_tab_view"))(window);initial_tabs=tab_count();
 auto popup=source->create_popup;source->create_popup=[popup](const std::string&uri){
  child=popup?popup(uri):nullptr;
  double ms=(g_get_monotonic_time()-clicked)/1000.;g_print("POPUP_VISIBLE_FEEDBACK_MS %.1f\n",ms);if(ms>400)fail("popup feedback exceeded 400ms");
  return child;
 };
 started=changed=g_get_monotonic_time();g_timeout_add(20,tick,nullptr);return G_SOURCE_REMOVE;
}
__attribute__((constructor))static void setup(){char exe[2048];auto n=readlink("/proc/self/exe",exe,sizeof(exe)-1);if(n<0)return;exe[n]=0;if(strstr(exe,"/.gnosis-wrapped"))g_timeout_add_seconds(3,start,nullptr);}
