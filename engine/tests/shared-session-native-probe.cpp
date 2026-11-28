/* SPDX-License-Identifier: GPL-3.0-or-later */
// Test-only translation unit: exercise the real private Session without production hooks.
#define gnosis_shared_start shared_native_start
#define gnosis_shared_stop shared_native_stop
#define gnosis_shared_open_uri shared_native_open_uri
#include "../../src/gnosis-shared.cpp"
#include <adwaita.h>
#include <unistd.h>
#include "../../embed/ephy-embed-container.h"
#include <execinfo.h>
#include <signal.h>
#include <dlfcn.h>

namespace {
std::string directory;
std::shared_ptr<GnosisBrave::Page> initial;
bool busy=false,direct=false;
GtkWindow* primary=nullptr;
gint64 last_tick=0,max_tick=0;unsigned heartbeats=0;
void reply(const std::string& value) {
  auto path=directory+"/reply.json";
  g_file_set_contents(path.c_str(),value.data(),value.size(),nullptr);
  busy=false;
}
GtkWidget* button(GtkWidget* widget,const std::string& text) {
  if(GTK_IS_BUTTON(widget)) {
    auto* label=gtk_button_get_label(GTK_BUTTON(widget));
    auto* tip=gtk_widget_get_tooltip_text(widget);
    if((label&&text==label)||(tip&&text==tip))return widget;
  }
  for(auto* child=gtk_widget_get_first_child(widget);child;child=gtk_widget_get_next_sibling(child))
    if(auto* found=button(child,text))return found;
  return nullptr;
}
void observe_transport() {
  direct=false;
  if(!GnosisShare::current||!GnosisShare::current->transport)return;
  auto previous=GnosisShare::current->transport->status;
  GnosisShare::current->transport->status=[previous](const std::string& text){if(text=="Connected peer-to-peer")direct=true;if(previous)previous(text);};
}
struct Feedback {std::string label;bool absent;gint64 started;GtkWidget* paint;double command_ms;gulong signal=0;};
void feedback(const std::string& label,bool absent,gint64 started) {
  GtkWidget* paint=GTK_WIDGET(primary);
  if(!absent&&label!="resize")
    for(auto* w=gtk_application_get_windows(GTK_APPLICATION(g_application_get_default()));w;w=w->next)
      if(auto* target=button(GTK_WIDGET(w->data),label)){paint=GTK_WIDGET(gtk_widget_get_root(target));break;}
  auto* clock=gtk_widget_get_frame_clock(paint);
  if(!clock){reply("{\"error\":\"No GTK frame clock\"}");return;}
  auto* watch=new Feedback{label,absent,started,GTK_WIDGET(g_object_ref(paint)),(g_get_monotonic_time()-started)/1000.};
  watch->signal=g_signal_connect_data(clock,"after-paint",G_CALLBACK(+[](GdkFrameClock* clock,gpointer data){
    auto* watch=static_cast<Feedback*>(data);bool visible=false;
    for(auto* w=gtk_application_get_windows(GTK_APPLICATION(g_application_get_default()));w;w=w->next) {
      if(watch->label=="resize") {
        if(ADW_IS_APPLICATION_WINDOW(w->data))
          if(auto* d=adw_application_window_get_visible_dialog(ADW_APPLICATION_WINDOW(w->data));d&&ADW_IS_ALERT_DIALOG(d))
            visible=gtk_widget_get_mapped(GTK_WIDGET(d));
      } else if(auto* found=button(GTK_WIDGET(w->data),watch->label))visible=gtk_widget_get_mapped(found);
      if(visible)break;
    }
    auto elapsed=g_get_monotonic_time()-watch->started;
    if(visible!=watch->absent||elapsed>2000000) {
      auto signal=watch->signal;
      reply("{\"commandMs\":"+std::to_string(watch->command_ms)+",\"feedbackMs\":"+std::to_string(elapsed/1000.)+",\"visible\":"+(visible!=watch->absent?"true":"false")+"}");
      g_signal_handler_disconnect(clock,signal);
    }else{gtk_widget_queue_draw(watch->paint);gdk_frame_clock_request_phase(clock,GDK_FRAME_CLOCK_PHASE_AFTER_PAINT);}
  }),watch,+[](gpointer data,GClosure*){auto* watch=static_cast<Feedback*>(data);g_object_unref(watch->paint);delete watch;},GConnectFlags(0));
  gtk_widget_queue_draw(paint);gdk_frame_clock_request_phase(clock,GDK_FRAME_CLOCK_PHASE_AFTER_PAINT);
}
gboolean tick(gpointer) {
  using namespace GnosisShare;
  auto now=g_get_monotonic_time();if(last_tick)max_tick=std::max(max_tick,now-last_tick);last_tick=now;++heartbeats;
  if(busy)return G_SOURCE_CONTINUE;
  auto path=directory+"/command.json";
  g_autofree char* bytes=nullptr;
  if(!g_file_get_contents(path.c_str(),&bytes,nullptr,nullptr))return G_SOURCE_CONTINUE;
  unlink(path.c_str());busy=true;
  g_autoptr(JsonParser) parser=json_parser_new();
  try {
    if(!json_parser_load_from_data(parser,bytes,-1,nullptr))throw std::runtime_error("Invalid test command");
    auto* o=json_node_get_object(json_parser_get_root(parser));auto op=str(o,"op");
    auto p=current?current->page.lock():initial;
    if(op=="host") {max_tick=0;heartbeats=0;auto began=g_get_monotonic_time();interface->host_requested("Native host",str(o,"server"));observe_transport();feedback("Stop Sharing",false,began);}
    else if(op=="join") {max_tick=0;heartbeats=0;auto began=g_get_monotonic_time();interface->join_requested(str(o,"invite"),"Native guest");observe_transport();feedback("Stop Viewing",false,began);}
    else if(op=="state") {
      if(!current){reply("null");return G_SOURCE_CONTINUE;}
      std::string members="[";for(const auto& [id,m]:current->members){if(members.size()>1)members+=",";members+="{\"id\":"+quote(id)+",\"approved\":"+(m.approved?"true":"false")+",\"control\":"+(m.control?"true":"false")+"}";}members+="]";
      reply("{\"direct\":"+std::string(direct?"true":"false")+",\"heartbeatMaxMs\":"+std::to_string(max_tick/1000.)+",\"heartbeats\":"+std::to_string(heartbeats)+",\"ready\":"+std::string(current->ready?"true":"false")+",\"viewerReady\":"+(current->viewer_ready?"true":"false")+",\"stopped\":"+(current->stopped?"true":"false")+",\"context\":"+std::to_string(current->context)+",\"cache\":"+std::to_string(current->cache.size())+",\"width\":"+std::to_string(current->width)+",\"height\":"+std::to_string(current->height)+",\"invite\":"+quote(current->transport?current->transport->invitation().link():"")+",\"members\":"+members+"}");
    } else if(op=="approve") {interface->approved(str(o,"peer"));reply("true");}
    else if(op=="control") {interface->control_changed(str(o,"peer"),flag(o,"enabled"));reply("true");}
    else if(op=="zoom") {interface->zoom_changed(num(o,"factor"));reply("true");}
    else if(op=="stop") {
      auto began=g_get_monotonic_time();
      auto label=current&&current->host?"Stop Sharing":"Stop Viewing";bool clicked=false;
      auto* app=GTK_APPLICATION(g_application_get_default());
      for(auto* windows=gtk_application_get_windows(app);windows;windows=windows->next)
        if(auto* b=button(GTK_WIDGET(windows->data),label)){g_signal_emit_by_name(b,"clicked");clicked=true;break;}
      if(!clicked)throw std::runtime_error("Visible native stop button missing");
      feedback(label,true,began);
    } else if(op=="present") {
      auto* app=GTK_APPLICATION(g_application_get_default());
      GtkWindow* parent=nullptr;
      for(auto* w=gtk_application_get_windows(app);w;w=w->next)
        if(EPHY_IS_WINDOW(w->data)){parent=GTK_WINDOW(w->data);break;}
      interface->present(parent);reply("true");
    } else if(op=="close-dialog") {
      bool closed=false;
      for(auto* w=gtk_application_get_windows(GTK_APPLICATION(g_application_get_default()));w;w=w->next)
        if(ADW_IS_APPLICATION_WINDOW(w->data))
          if(auto* d=adw_application_window_get_visible_dialog(ADW_APPLICATION_WINDOW(w->data))){adw_dialog_close(d);closed=true;break;}
      if(!closed)throw std::runtime_error("Session dialog was not visible");
      reply("true");
    } else if(op=="resize-watch") {
      feedback("resize",false,g_get_monotonic_time());
    } else if(op=="resize-request") {
      interface->resize_requested(num(o,"width"),num(o,"height"));reply("true");
    } else if(op=="resize-reply") {
      bool answered=false;
      for(auto* w=gtk_application_get_windows(GTK_APPLICATION(g_application_get_default()));w;w=w->next)
        if(ADW_IS_APPLICATION_WINDOW(w->data))
          if(auto* d=adw_application_window_get_visible_dialog(ADW_APPLICATION_WINDOW(w->data));d&&ADW_IS_ALERT_DIALOG(d)) {
            g_signal_emit_by_name(d,"response",flag(o,"allow")?"approve":"deny");adw_dialog_close(d);answered=true;break;
          }
      reply(answered?"true":"false");
    } else if(op=="eval"||op=="cdp") {
      if(!p)throw std::runtime_error("No native page");
      auto method=op=="eval"?"Runtime.evaluate":str(o,"method");
      auto params=op=="eval"?"{\"expression\":"+quote(str(o,"expression",1024*1024))+",\"returnByValue\":true,\"awaitPromise\":true}":str(o,"params");
      p->command(method,params,[](JsonObject* result,const std::string& error){
        if(!result||!error.empty()){reply("{\"error\":"+GnosisShare::quote(error)+"}");return;}
        g_autoptr(JsonNode) n=json_node_new(JSON_NODE_OBJECT);json_node_set_object(n,result);reply(GnosisShare::encode(n));
      });
    } else if(op=="quit") {reply("true");g_application_quit(g_application_get_default());}
    else throw std::runtime_error("Unknown native test operation");
  }catch(const std::exception& error){reply("{\"error\":"+quote(error.what())+"}");}
  return G_SOURCE_CONTINUE;
}
gboolean start(gpointer) {
  auto* app=g_application_get_default();if(!app)return G_SOURCE_CONTINUE;
  auto* window=gtk_application_get_active_window(GTK_APPLICATION(app));if(!window||!EPHY_IS_WINDOW(window))return G_SOURCE_CONTINUE;
  auto* embed=ephy_window_get_active_embed(EPHY_WINDOW(window));if(!embed)return G_SOURCE_CONTINUE;
  primary=window;
  initial=ephy_engine_view_brave_page(EPHY_ENGINE_VIEW(ephy_embed_get_web_view(embed)));
  if(!initial||initial->loading||initial->session.empty())return G_SOURCE_CONTINUE;
  if(auto* dialog=adw_application_window_get_visible_dialog(ADW_APPLICATION_WINDOW(window)))adw_dialog_force_close(dialog);
  Dl_info library{};dladdr(reinterpret_cast<void*>(ephy_embed_container_get_children),&library);
  g_printerr("SHARED_NATIVE_LIBRARY %s\n",library.dli_fname);
  shared_native_start(app);
  g_file_set_contents((directory+"/ready").c_str(),"ready",-1,nullptr);
  g_timeout_add(20,tick,nullptr);return G_SOURCE_REMOVE;
}
__attribute__((constructor)) void setup() {
  char path[2048];auto n=readlink("/proc/self/exe",path,sizeof(path)-1);if(n<0)return;path[n]=0;
  const char* dir=g_getenv("GNOSIS_SHARED_NATIVE_DIRECTORY");
  if(dir&&strstr(path,"/.gnosis-wrapped")){directory=dir;signal(SIGABRT,+[](int){void* stack[40];int n=backtrace(stack,40);backtrace_symbols_fd(stack,n,2);_exit(134);});g_timeout_add(100,start,nullptr);}
}
}
