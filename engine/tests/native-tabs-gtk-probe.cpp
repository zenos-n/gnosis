/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "browser.hpp"
#include <adwaita.h>
#include <gdk/x11/gdkx.h>
#include <dlfcn.h>
#include <unistd.h>
#include <cstring>
#include <vector>
static GtkWindow *window;
static GtkWidget *tabs, *source_embed, *host, *preferences;
static AdwTabView *selection;
static AdwSwitchRow *foreground;
static std::shared_ptr<GnosisBrave::Page> source, child;
static int stage, scenario, initial_tabs, selection_changes;
static bool busy, tracking, startup_checked;
static gint64 started, changed, clicked;
static void fail(const char *message){g_printerr("NATIVE_TABS_FAIL scenario=%d stage=%d %s\n",scenario,stage,message);_exit(77);}
static GtkWidget *find(GtkWidget *widget,const char *type){if(!strcmp(G_OBJECT_TYPE_NAME(widget),type))return widget;for(auto *c=gtk_widget_get_first_child(widget);c;c=gtk_widget_get_next_sibling(c))if(auto *r=find(c,type))return r;return nullptr;}
static void *active(){return reinterpret_cast<void *(*)(void *)>(dlsym(RTLD_DEFAULT,"ephy_window_get_active_embed"))(window);}
static std::shared_ptr<GnosisBrave::Page> page(void *embed){auto *v=reinterpret_cast<void *(*)(void *)>(dlsym(RTLD_DEFAULT,"ephy_embed_get_web_view"))(embed);return reinterpret_cast<std::shared_ptr<GnosisBrave::Page>(*)(void *)>(dlsym(RTLD_DEFAULT,"_Z27ephy_engine_view_brave_pageP15_EphyEngineView"))(v);}
static int count(){return reinterpret_cast<int(*)(void *)>(dlsym(RTLD_DEFAULT,"ephy_tab_view_get_n_pages"))(tabs);}
static void evaluate(const std::string &js){busy=true;source->command("Runtime.evaluate","{\"expression\":"+GnosisBrave::quote(js)+",\"returnByValue\":true}",[](JsonObject *r,const std::string &e){busy=false;if(!r||!e.empty()||json_object_has_member(r,"exceptionDetails"))fail("fixture evaluation failed");});}
static void click(){graphene_point_t p{40,120},mapped;if(!gtk_widget_compute_point(host,GTK_WIDGET(window),&p,&mapped))fail("coordinate mapping");auto id=std::to_string(gdk_x11_surface_get_xid(gtk_native_get_surface(GTK_NATIVE(window))));std::vector<std::string> args={g_getenv("GNOSIS_XDOTOOL"),"windowfocus",id,"mousemove","--window",id,std::to_string(int(mapped.x)),std::to_string(int(mapped.y)),"click","2"};std::vector<char*> argv;for(auto &s:args)argv.push_back(s.data());argv.push_back(nullptr);int status=0;clicked=g_get_monotonic_time();if(!g_spawn_sync(nullptr,argv.data(),nullptr,G_SPAWN_DEFAULT,nullptr,nullptr,nullptr,nullptr,&status,nullptr)||status)fail("XTest middle click");}
static gboolean tick(gpointer){
 auto now=g_get_monotonic_time();if(now-started>50*G_USEC_PER_SEC)fail("timeout");if(busy||source->loading||source->native_tab_id()<0||now-changed<150000)return G_SOURCE_CONTINUE;
 if(auto *dialog=adw_application_window_get_visible_dialog(ADW_APPLICATION_WINDOW(window))){adw_dialog_force_close(dialog);return G_SOURCE_CONTINUE;}
 if(!startup_checked){busy=true;GnosisBrave::browser_command(g_getenv("GNOSIS_TABS_PRIVATE")!=nullptr,"Target.getTargets","{}",[](JsonObject *r,const std::string &e){busy=false;if(!r||!e.empty())fail("native startup inspection failed");auto *targets=json_object_get_array_member(r,"targetInfos");int pages=0;for(guint i=0;i<json_array_get_length(targets);++i){auto *item=json_array_get_object_element(targets,i);if(!strcmp(json_object_get_string_member(item,"type"),"page"))++pages;}if(pages!=1)fail("Brave independently restored native tabs");startup_checked=true;g_print("NATIVE_TABS_STARTUP_PASS one GTK tab and one native page\n");});return G_SOURCE_CONTINUE;}
 if(stage==0){adw_switch_row_set_active(foreground,scenario==3);evaluate("document.body.innerHTML='<a style=\"position:absolute;left:20px;top:100px;width:200px;height:40px\" href=\"/opened\" "+std::string(scenario==1?"target=_blank rel=noopener":scenario==2?"target=_blank rel=opener":"")+">Open in background</a>'");}
 else if(stage==1){selection_changes=0;tracking=true;click();}
 else if(stage==2){
  if(count()<initial_tabs+scenario+1)return G_SOURCE_CONTINUE;if(count()!=initial_tabs+scenario+1)fail("duplicate GTK tabs");
  bool switched=active()!=source_embed;
  if(switched!=(scenario==3)||(scenario<3&&selection_changes))fail("middle click stole focus or ignored explicit opt-in");
  g_print("NATIVE_TABS_OPEN_PASS scenario=%d selected-new=%d visible-feedback=%.1fms\n",scenario,switched,(now-clicked)/1000.);
  tracking=false;reinterpret_cast<void(*)(void*,int)>(dlsym(RTLD_DEFAULT,"ephy_tab_view_select_nth_page"))(tabs,count()-1);child=page(active());
 }
 else if(stage==3){if(!child||child==source||child->loading||child->native_tab_id()<0||!child->uri.ends_with("/opened"))return G_SOURCE_CONTINUE;reinterpret_cast<bool(*)(void*,void*)>(dlsym(RTLD_DEFAULT,"ephy_tab_view_select_page"))(tabs,source_embed);child.reset();}
 else if(++scenario<4){stage=0;changed=now;return G_SOURCE_CONTINUE;}
 else{source.reset();g_clear_object(&preferences);g_print("NATIVE_TABS_PASS plain/noopener/opener middle clicks stay in background; explicit foreground option works\n");g_action_group_activate_action(G_ACTION_GROUP(g_application_get_default()),"quit",nullptr);return G_SOURCE_REMOVE;}
 ++stage;changed=now;return G_SOURCE_CONTINUE;
}
static gboolean start(gpointer){
 auto *app=g_application_get_default();if(!app)return G_SOURCE_CONTINUE;window=gtk_application_get_active_window(GTK_APPLICATION(app));if(!window||!active())return G_SOURCE_CONTINUE;source_embed=GTK_WIDGET(active());source=page(source_embed);host=find(source_embed,"GnosisCompositorView");if(!host||!source)return G_SOURCE_CONTINUE;
 tabs=reinterpret_cast<GtkWidget*(*)(void*)>(dlsym(RTLD_DEFAULT,"ephy_window_get_tab_view"))(window);selection=ADW_TAB_VIEW(find(tabs,"AdwTabView"));if(!selection)fail("GTK tab view missing");initial_tabs=count();if(initial_tabs!=1)fail("tabs restored despite default-off policy");
 auto type=reinterpret_cast<GType(*)()>(dlsym(RTLD_DEFAULT,"prefs_general_page_get_type"))();preferences=GTK_WIDGET(g_object_ref_sink(g_object_new(type,nullptr)));foreground=ADW_SWITCH_ROW(gtk_widget_get_template_child(preferences,type,"enable_switch_to_new_tab"));auto *restore=ADW_SWITCH_ROW(gtk_widget_get_template_child(preferences,type,"restore_session_row"));
 if(adw_switch_row_get_active(foreground)||adw_switch_row_get_active(restore))fail("background tabs and session restore must default off");
 adw_switch_row_set_active(restore,TRUE);auto *settings=reinterpret_cast<GSettings *(*)(const char *)>(dlsym(RTLD_DEFAULT,"ephy_settings_get"))("com.neg-zero.Gnosis");g_autofree char *policy=g_settings_get_string(settings,"restore-session-policy");if(strcmp(policy,"always"))fail("restore opt-in failed");adw_switch_row_set_active(restore,FALSE);
 g_signal_connect(selection,"notify::selected-page",G_CALLBACK(+[](GObject*,GParamSpec*,gpointer){if(tracking)++selection_changes;}),nullptr);
 started=changed=g_get_monotonic_time();g_timeout_add(20,tick,nullptr);return G_SOURCE_REMOVE;
}
__attribute__((constructor))static void setup(){char exe[2048];auto n=readlink("/proc/self/exe",exe,sizeof(exe)-1);if(n<0)return;exe[n]=0;if(strstr(exe,"/.gnosis-wrapped"))g_timeout_add_seconds(3,start,nullptr);}
