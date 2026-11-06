/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "browser.hpp"
#include "ephy-web-app-dialog.h"
#include "web-app-config.h"
#include <adwaita.h>
#include <dlfcn.h>
#include <unistd.h>
#include <cstdio>
#include <cstring>
static std::shared_ptr<GnosisBrave::Page> page;
static GtkWindow *window;
static std::string last;
static bool checked;
static EphyWebAppDialog *setup;
static void fail(const char*s){g_printerr("WEB_APP_LINKS_FAIL %s\n",s);_exit(77);}
static GtkWidget *row(GtkWidget*w,const char*title){if(ADW_IS_PREFERENCES_ROW(w)&&!g_strcmp0(adw_preferences_row_get_title(ADW_PREFERENCES_ROW(w)),title))return w;for(auto*c=gtk_widget_get_first_child(w);c;c=gtk_widget_get_next_sibling(c))if(auto*r=row(c,title))return r;return nullptr;}
static bool check_ui(){
  if(!setup){
  auto*config=g_key_file_new();auto*icon=g_themed_icon_new("folder");
  setup=EPHY_WEB_APP_DIALOG(ephy_web_app_dialog_new("Link fixture","https://x.com",icon,config,FALSE));g_object_ref_sink(setup);g_object_unref(icon);g_key_file_unref(config);
  adw_dialog_present(ADW_DIALOG(setup),GTK_WIDGET(window));return false;
  }
  auto*d=setup;if(!gtk_widget_get_mapped(GTK_WIDGET(d)))return false;
  auto*enabled=row(GTK_WIDGET(d),"Open Links in This App");auto*domains=row(GTK_WIDGET(d),"Domains (comma-separated)");
  if(!ADW_IS_SWITCH_ROW(enabled)||!ADW_IS_ENTRY_ROW(domains)||adw_switch_row_get_active(ADW_SWITCH_ROW(enabled)))fail("native link controls/default missing");
  if(strcmp(gtk_editable_get_text(GTK_EDITABLE(domains)),"x.com"))fail("site domain not prefilled");
  adw_switch_row_set_active(ADW_SWITCH_ROW(enabled),true);gtk_editable_set_text(GTK_EDITABLE(domains),"x.com, twitter.com");
  auto*saved=ephy_web_app_dialog_dup_config(d);if(!gnosis_web_app_config_validate(saved,nullptr)||!gnosis_web_app_link_match(saved,"https://twitter.com/post"))fail("native aliases not persisted");
  g_key_file_unref(saved);adw_dialog_force_close(ADW_DIALOG(d));g_object_unref(d);setup=nullptr;g_print("WEB_APP_LINKS_UI_PASS native switch and domain entry\n");return true;
}
static gboolean tick(gpointer){
  auto e=reinterpret_cast<void*(*)(void*)>(dlsym(RTLD_DEFAULT,"ephy_window_get_active_embed"));auto v=reinterpret_cast<void*(*)(void*)>(dlsym(RTLD_DEFAULT,"ephy_embed_get_web_view"));auto p=reinterpret_cast<std::shared_ptr<GnosisBrave::Page>(*)(void*)>(dlsym(RTLD_DEFAULT,"_Z27ephy_engine_view_brave_pageP15_EphyEngineView"));
  if(e(window))page=p(v(e(window)));
  if(page->session.empty()||page->loading||page->uri.empty())return G_SOURCE_CONTINUE;
  if(!checked){if(!check_ui())return G_SOURCE_CONTINUE;checked=true;}
  if(page->uri!=last){
    last=page->uri;
    FILE*f=fopen(g_getenv("GNOSIS_WEB_APP_LINK_REPORT"),"a");if(!f)fail("report unavailable");
    auto*application=G_APPLICATION(g_application_get_default());
    g_autofree char*encoded=g_base64_encode(reinterpret_cast<const guchar*>(last.data()),last.size());
    fprintf(f,"%d\t%s\t%d\t%s\n",int(getpid()),g_application_get_application_id(application),g_list_length(gtk_application_get_windows(GTK_APPLICATION(application))),encoded);fclose(f);
  }
  return G_SOURCE_CONTINUE;
}
static gboolean start(gpointer){
  auto*a=g_application_get_default();if(!GTK_IS_APPLICATION(a))return G_SOURCE_CONTINUE;
  window=gtk_application_get_active_window(GTK_APPLICATION(a));if(!window)return G_SOURCE_CONTINUE;
  auto*d=ADW_IS_APPLICATION_WINDOW(window)?adw_application_window_get_visible_dialog(ADW_APPLICATION_WINDOW(window)):nullptr;
  if(d&&ADW_IS_ALERT_DIALOG(d)){g_signal_emit_by_name(d,"response","no");adw_dialog_close(d);return G_SOURCE_CONTINUE;}
  auto e=reinterpret_cast<void*(*)(void*)>(dlsym(RTLD_DEFAULT,"ephy_window_get_active_embed"));auto v=reinterpret_cast<void*(*)(void*)>(dlsym(RTLD_DEFAULT,"ephy_embed_get_web_view"));auto p=reinterpret_cast<std::shared_ptr<GnosisBrave::Page>(*)(void*)>(dlsym(RTLD_DEFAULT,"_Z27ephy_engine_view_brave_pageP15_EphyEngineView"));
  if(!e(window))return G_SOURCE_CONTINUE;page=p(v(e(window)));g_timeout_add(100,tick,nullptr);return G_SOURCE_REMOVE;
}
__attribute__((constructor))static void initialize_probe(){char exe[2048];auto n=readlink("/proc/self/exe",exe,sizeof(exe)-1);if(n<0)return;exe[n]=0;if(strstr(exe,"/.gnosis-wrapped"))g_timeout_add_seconds(3,start,nullptr);}
