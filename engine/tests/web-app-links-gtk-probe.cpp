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
static GtkWidget *find_type(GtkWidget*w,GType type){if(g_type_is_a(G_OBJECT_TYPE(w),type))return w;for(auto*c=gtk_widget_get_first_child(w);c;c=gtk_widget_get_next_sibling(c))if(auto*r=find_type(c,type))return r;return nullptr;}
static GtkWidget *chip(GtkWidget*w,const char*domain){for(auto*c=gtk_widget_get_first_child(w);c;c=gtk_widget_get_next_sibling(c))if(!g_strcmp0(static_cast<const char*>(g_object_get_data(G_OBJECT(c),"domain")),domain))return c;return nullptr;}
static void capture(GtkWidget*w){auto*p=gtk_widget_paintable_new(w);auto*s=gtk_snapshot_new();gdk_paintable_snapshot(p,GDK_SNAPSHOT(s),gtk_widget_get_width(w),gtk_widget_get_height(w));auto*n=gtk_snapshot_free_to_node(s);if(n){auto*t=gsk_renderer_render_texture(gtk_native_get_renderer(gtk_widget_get_native(w)),n,nullptr);gdk_texture_save_to_png(t,"/tmp/gnosis-domain-chips.png");g_object_unref(t);gsk_render_node_unref(n);}g_object_unref(p);}
static unsigned count(GtkWidget*w){unsigned n=0;for(auto*c=gtk_widget_get_first_child(w);c;c=gtk_widget_get_next_sibling(c))n++;return n;}
static bool accepted;
static void on_accepted(){accepted=true;}
static bool check_ui(){
  static unsigned stage;
  if(!setup){
    auto*config=g_key_file_new();auto*icon=g_themed_icon_new("folder");
    setup=EPHY_WEB_APP_DIALOG(ephy_web_app_dialog_new("Link fixture","https://example.com",icon,config,FALSE));g_object_ref_sink(setup);g_object_unref(icon);g_key_file_unref(config);
    adw_dialog_set_content_width(ADW_DIALOG(setup),360);
    adw_dialog_present(ADW_DIALOG(setup),GTK_WIDGET(window));return false;
  }
  auto*d=setup;if(!gtk_widget_get_mapped(GTK_WIDGET(d)))return false;
  auto*enabled=row(GTK_WIDGET(d),"Open Links in This App");auto*entry=row(GTK_WIDGET(d),"Add Domain");auto*domains=find_type(GTK_WIDGET(d),ADW_TYPE_WRAP_BOX);
  if(!stage){
    if(!ADW_IS_SWITCH_ROW(enabled)||!ADW_IS_ENTRY_ROW(entry)||!domains||adw_switch_row_get_active(ADW_SWITCH_ROW(enabled)))fail("native link controls/default missing");
    if(!chip(domains,"example.com")||count(domains)!=1||gtk_widget_is_sensitive(domains))fail("site domain not prefilled/disabled");
    adw_switch_row_set_active(ADW_SWITCH_ROW(enabled),true);
    gtk_editable_set_text(GTK_EDITABLE(entry)," CHAT.EXAMPLE.ORG ");g_signal_emit_by_name(entry,"entry-activated");
    if(!chip(domains,"chat.example.org")||*gtk_editable_get_text(GTK_EDITABLE(entry)))fail("Enter did not add normalized chip");
    gtk_editable_set_text(GTK_EDITABLE(entry),"chat.example.org");g_signal_emit_by_name(entry,"entry-activated");
    if(count(domains)!=2)fail("duplicate chip added");
    gtk_editable_set_text(GTK_EDITABLE(entry),"https://invalid.example/path");g_signal_emit_by_name(entry,"entry-activated");
    if(count(domains)!=2||!gtk_widget_has_css_class(entry,"error"))fail("invalid domain accepted");
    gtk_editable_set_text(GTK_EDITABLE(entry),"temporary.example");
    auto find_add=[](auto&& self,GtkWidget*w)->GtkWidget*{if(GTK_IS_BUTTON(w)&&!g_strcmp0(gtk_button_get_icon_name(GTK_BUTTON(w)),"list-add-symbolic"))return w;for(auto*c=gtk_widget_get_first_child(w);c;c=gtk_widget_get_next_sibling(c))if(auto*r=self(self,c))return r;return nullptr;};
    auto*add=find_add(find_add,entry);if(!add)fail("missing plus button");g_signal_emit_by_name(add,"clicked");
    if(!chip(domains,"temporary.example")||gtk_widget_has_css_class(entry,"error"))fail("plus button did not add/clear error");
    g_signal_emit_by_name(chip(domains,"temporary.example"),"clicked");if(count(domains)!=2)fail("chip removal failed");
    gtk_editable_set_text(GTK_EDITABLE(entry),"another.example.net");g_signal_emit_by_name(entry,"entry-activated");
    stage=1;return false;
  }
  auto*first=gtk_widget_get_first_child(domains);auto*last=gtk_widget_get_last_child(domains);graphene_rect_t a,b;
  if(!gtk_widget_compute_bounds(first,domains,&a)||!gtk_widget_compute_bounds(last,domains,&b)||b.origin.y<=a.origin.y)fail("domain chips did not wrap at narrow width");
  capture(GTK_WIDGET(d));
  gtk_editable_set_text(GTK_EDITABLE(entry),"pending.example");
  g_signal_connect(d,"accepted",G_CALLBACK(on_accepted),nullptr);
  auto*save=find_type(GTK_WIDGET(d),GTK_TYPE_BUTTON);
  // Find the actual Save button, excluding the icon and domain controls.
  auto find_save=[](auto&& self,GtkWidget*w)->GtkWidget*{if(GTK_IS_BUTTON(w)&&!g_strcmp0(gtk_button_get_label(GTK_BUTTON(w)),"Save"))return w;for(auto*c=gtk_widget_get_first_child(w);c;c=gtk_widget_get_next_sibling(c))if(auto*r=self(self,c))return r;return nullptr;};
  save=find_save(find_save,GTK_WIDGET(d));if(!save)fail("missing Save button");g_signal_emit_by_name(save,"clicked");
  if(!accepted||!chip(domains,"pending.example"))fail("Save dropped pending domain");
  auto*saved=ephy_web_app_dialog_dup_config(d);if(!gnosis_web_app_config_validate(saved,nullptr)||!gnosis_web_app_link_match(saved,"https://chat.example.org/post")||!gnosis_web_app_link_match(saved,"https://pending.example/"))fail("native aliases not persisted");
  g_key_file_unref(saved);adw_dialog_force_close(ADW_DIALOG(d));g_object_unref(d);setup=nullptr;g_print("WEB_APP_LINKS_UI_PASS wrapped chips, Enter/plus, duplicate/invalid input, removal and Save\n");return true;
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
