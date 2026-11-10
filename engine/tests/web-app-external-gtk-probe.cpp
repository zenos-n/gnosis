/* SPDX-License-Identifier: GPL-3.0-or-later
 * Real middle/right clicks in a disposable web app; observe actual receivers. */
#include "browser.hpp"
#include <adwaita.h>
#include <gdk/x11/gdkx.h>
#include <dlfcn.h>
#include <unistd.h>
#include <cstdio>
#include <cstring>
#include <fstream>
static GtkWindow* source;
static std::string last,command,pending;
static bool busy;
static void fail(const char*s){g_printerr("WEB_APP_EXTERNAL_FAIL %s\n",s);_exit(77);}
static std::shared_ptr<GnosisBrave::Page> page(GtkWindow*w){
 auto e=reinterpret_cast<void*(*)(void*)>(dlsym(RTLD_DEFAULT,"ephy_window_get_active_embed"));
 auto v=reinterpret_cast<void*(*)(void*)>(dlsym(RTLD_DEFAULT,"ephy_embed_get_web_view"));
 auto p=reinterpret_cast<std::shared_ptr<GnosisBrave::Page>(*)(void*)>(dlsym(RTLD_DEFAULT,"_Z27ephy_engine_view_brave_pageP15_EphyEngineView"));
 return e(w)?p(v(e(w))):nullptr;
}
static GtkWidget* item(GtkWidget*w,const char*label){
 if(!strcmp(G_OBJECT_TYPE_NAME(w),"GtkModelButton")){
  auto has=[](auto&&self,GtkWidget*n,const char*t)->bool{if(GTK_IS_LABEL(n)&&!g_strcmp0(gtk_label_get_text(GTK_LABEL(n)),t))return true;for(auto*c=gtk_widget_get_first_child(n);c;c=gtk_widget_get_next_sibling(c))if(self(self,c,t))return true;return false;};
  if(has(has,w,label))return w;
 }
 for(auto*c=gtk_widget_get_first_child(w);c;c=gtk_widget_get_next_sibling(c))if(auto*r=item(c,label))return r;return nullptr;
}
static void run(std::vector<std::string>args){std::vector<char*>argv;for(auto&s:args)argv.push_back(s.data());argv.push_back(nullptr);int status=0;if(!g_spawn_sync(nullptr,argv.data(),nullptr,G_SPAWN_DEFAULT,nullptr,nullptr,nullptr,nullptr,&status,nullptr)||status)fail("XTest");}
static gboolean tick(gpointer){
 auto*a=g_application_get_default();if(!GTK_IS_APPLICATION(a))return G_SOURCE_CONTINUE;
 auto*windows=gtk_application_get_windows(GTK_APPLICATION(a));if(!windows)return G_SOURCE_CONTINUE;
 if(!source)source=GTK_WINDOW(windows->data);
 for(auto*w=windows;w;w=w->next)if(auto*d=adw_application_window_get_visible_dialog(ADW_APPLICATION_WINDOW(w->data))){if(ADW_IS_ALERT_DIALOG(d)){g_signal_emit_by_name(d,"response","no");adw_dialog_force_close(d);}}
 std::string report;
 for(auto*w=windows;w;w=w->next){auto p=page(GTK_WINDOW(w->data));if(!p||p->loading||p->uri.empty())continue;g_autofree char*url=g_base64_encode(reinterpret_cast<const guchar*>(p->uri.data()),p->uri.size());auto children=reinterpret_cast<GList*(*)(void*)>(dlsym(RTLD_DEFAULT,"ephy_embed_container_get_children"))(w->data);g_autofree char*line=g_strdup_printf("%d\t%s\t%d\t%p\t%s\t%d\n",int(getpid()),g_application_get_application_id(a),g_list_length(windows),w->data,url,g_list_length(children));g_list_free(children);report+=line;}
 if(report!=last){FILE*f=fopen(g_getenv("GNOSIS_WEB_APP_EXTERNAL_REPORT"),"a");if(!f)fail("report");fwrite(report.data(),1,report.size(),f);fclose(f);last=report;}
 if(strcmp(g_application_get_application_id(a),"com.neg-zero.Gnosis.WebApp_externalfixture")||busy)return G_SOURCE_CONTINUE;
 if(!pending.empty()){
  auto*b=item(GTK_WIDGET(source),pending.c_str());if(!b)return G_SOURCE_CONTINUE;
  if(!gtk_widget_activate(b))fail("native context item activation");pending.clear();g_printerr("WEB_APP_EXTERNAL_DONE %s\n",command.c_str());return G_SOURCE_CONTINUE;
 }
 std::ifstream in(g_getenv("GNOSIS_WEB_APP_EXTERNAL_COMMAND"));std::string next;getline(in,next);if(next.empty()||next==command)return G_SOURCE_CONTINUE;
 auto p=page(source);if(!p||p->loading||p->session.empty())return G_SOURCE_CONTINUE;
 command=next;busy=true;
 const std::string id=command=="middle-internal"?"internal":command=="middle-alias"?"alias":"external";
 p->command("Runtime.evaluate","{\"expression\":"+GnosisBrave::quote("(()=>{const r=document.getElementById('"+id+"').getBoundingClientRect();return [r.x+r.width/2,r.y+r.height/2]})()")+",\"returnByValue\":true}",[p](JsonObject*r,const std::string&e){
  busy=false;if(!r||!e.empty())fail("link position");auto*v=json_object_get_object_member(r,"result");auto*coords=json_object_get_array_member(v,"value");
  graphene_point_t from{float(json_array_get_double_element(coords,0)),float(json_array_get_double_element(coords,1))},to;
  if(!gtk_widget_compute_point(p->widget(),GTK_WIDGET(source),&from,&to))fail("widget coordinates");
  const std::string tool=g_getenv("GNOSIS_XDOTOOL");const auto xid=std::to_string(gdk_x11_surface_get_xid(gtk_native_get_surface(GTK_NATIVE(source))));
  run({tool,"windowraise",xid});run({tool,"windowfocus",xid});run({tool,"mousemove","--window",xid,std::to_string(int(to.x)),std::to_string(int(to.y))});
  if(command.starts_with("middle")){run({tool,"click","2"});g_printerr("WEB_APP_EXTERNAL_DONE %s\n",command.c_str());}
  else {pending=command=="window-external"?"Open Link in New Window":"Open Link in New Tab";run({tool,"click","3"});}
 });
 return G_SOURCE_CONTINUE;
}
__attribute__((constructor))static void init(){char exe[2048];auto n=readlink("/proc/self/exe",exe,sizeof(exe)-1);if(n<0)return;exe[n]=0;if(strstr(exe,"/.gnosis-wrapped"))g_timeout_add(200,tick,nullptr);}
