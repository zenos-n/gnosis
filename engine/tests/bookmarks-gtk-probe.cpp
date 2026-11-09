/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "ephy-bookmark.h"
#include "ephy-bookmarks-manager.h"
#include "ephy-bookmarks-dialog.h"
#include <adwaita.h>
#include <gdk/x11/gdkx.h>
#include <dlfcn.h>
#include <unistd.h>
#include <cstring>
static GtkWidget *sidebar;
static GtkStack *stack;
static GtkListBox *details, *root_list;
static GtkSearchEntry *search;
static unsigned stage;
static gint64 due;
static GtkStackTransitionType last_transition;
extern "C" void gtk_stack_set_visible_child_full(GtkStack*s,const char*name,GtkStackTransitionType type){auto real=reinterpret_cast<void(*)(GtkStack*,const char*,GtkStackTransitionType)>(dlsym(RTLD_NEXT,"gtk_stack_set_visible_child_full"));if(s==stack)last_transition=type;real(s,name,type);}
static void fail(const char*s){g_printerr("BOOKMARKS_GTK_FAIL stage=%u %s\n",stage,s);_exit(77);}
static GtkWidget* find(GtkWidget*w,const char*type){if(!strcmp(G_OBJECT_TYPE_NAME(w),type))return w;for(auto*c=gtk_widget_get_first_child(w);c;c=gtk_widget_get_next_sibling(c))if(auto*r=find(c,type))return r;return nullptr;}
static GtkWidget* child(const char*name){return GTK_WIDGET(gtk_widget_get_template_child(sidebar,EPHY_TYPE_BOOKMARKS_DIALOG,name));}
static unsigned count(GtkListBox*list){unsigned n=0;while(gtk_list_box_get_row_at_index(list,n))n++;return n;}
static void capture(const char*name){
 const char*tool=g_getenv("GNOSIS_CAPTURE_TOOL");if(!tool)return;
 g_autofree char*id=g_strdup_printf("%lu",gdk_x11_surface_get_xid(gtk_native_get_surface(gtk_widget_get_native(sidebar))));
 g_autofree char*path=g_strdup_printf("/tmp/gnosis-bookmarks-screen-%s.png",name);
 const char*args[]={tool,"-window",id,path,nullptr};int status=0;
 if(!g_spawn_sync(nullptr,const_cast<char**>(args),nullptr,G_SPAWN_DEFAULT,nullptr,nullptr,nullptr,nullptr,&status,nullptr)||!g_spawn_check_wait_status(status,nullptr))fail("window capture failed");
}
static void open_category(const char*name){for(unsigned i=0;auto*r=gtk_list_box_get_row_at_index(root_list,i);i++)if(!g_strcmp0(adw_preferences_row_get_title(ADW_PREFERENCES_ROW(r)),name)){g_signal_emit_by_name(r,"activated");return;}fail("category missing");}
static void back(){if(!gtk_widget_activate_action(sidebar,"dialog.tag-detail-back",nullptr))fail("missing back action");}
static void expect_page(const char*name){if(g_strcmp0(gtk_stack_get_visible_child_name(stack),name))fail(name);}
static void verify_row(){auto*r=gtk_list_box_get_row_at_index(details,0);if(!G_TYPE_CHECK_INSTANCE_TYPE(r,g_type_from_name("EphyBookmarkRow"))||!*adw_action_row_get_subtitle(ADW_ACTION_ROW(r)))fail("missing site subtitle");auto*i=GTK_IMAGE(gtk_widget_get_template_child(GTK_WIDGET(r),g_type_from_name("EphyBookmarkRow"),"favicon_image"));if(!gtk_image_get_gicon(i)||!gtk_widget_get_mapped(GTK_WIDGET(i)))fail("missing favicon fallback");}
static gboolean tick(gpointer){
 if(g_get_monotonic_time()<due)return G_SOURCE_CONTINUE;
 due=g_get_monotonic_time()+450000;
 switch(stage++){
 case 0:capture("root");open_category("Favorites");if(last_transition!=GTK_STACK_TRANSITION_TYPE_SLIDE_LEFT||!gtk_stack_get_transition_running(stack))fail("missing forward slide");due=g_get_monotonic_time()+90000;break;
 case 1:if(!gtk_stack_get_transition_running(stack)||count(details)!=2)fail("forward page lost contents");capture("entering");break;
 case 2:expect_page("tag_detail");verify_row();capture("favorites");back();if(last_transition!=GTK_STACK_TRANSITION_TYPE_SLIDE_RIGHT||!gtk_stack_get_transition_running(stack)||count(details)!=2)fail("back slide lost outgoing rows");due=g_get_monotonic_time()+90000;break;
 case 3:if(!gtk_stack_get_transition_running(stack)||count(details)!=2)fail("outgoing category cleared early");capture("leaving");break;
 case 4:expect_page("default");if(count(details))fail("outgoing rows not retired");open_category("Research");break;
 case 5:if(count(details)!=1)fail("reopening category duplicated old rows");verify_row();gtk_editable_set_text(GTK_EDITABLE(search),"no-such-bookmark");break;
 case 6:expect_page("empty-state");gtk_editable_set_text(GTK_EDITABLE(search),"");break;
 case 7:expect_page("tag_detail");back();break;
 case 8:gtk_editable_set_text(GTK_EDITABLE(search),"Trainsworld");break;
 case 9:expect_page("searching_bookmarks");gtk_editable_set_text(GTK_EDITABLE(search),"");break;
 case 10:expect_page("default");ephy_bookmarks_dialog_set_is_editing(EPHY_BOOKMARKS_DIALOG(sidebar),TRUE);if(gtk_widget_get_visible(child("edit_button"))||!gtk_widget_get_visible(child("done_button")))fail("edit controls broken");open_category("Favorites");break;
 case 11:if(count(details)!=2)fail("edit/category row count");ephy_bookmarks_dialog_set_is_editing(EPHY_BOOKMARKS_DIALOG(sidebar),FALSE);back();break;
 case 12:g_object_set(gtk_settings_get_default(),"gtk-enable-animations",FALSE,nullptr);open_category("Favorites");if(gtk_stack_get_transition_running(stack))fail("ignored reduced motion");back();if(count(details))fail("reduced-motion cleanup missing");gtk_widget_set_direction(sidebar,GTK_TEXT_DIR_RTL);g_object_set(gtk_settings_get_default(),"gtk-enable-animations",TRUE,nullptr);open_category("Favorites");if(last_transition!=GTK_STACK_TRANSITION_TYPE_SLIDE_RIGHT)fail("RTL forward direction");break;
 case 13:back();if(last_transition!=GTK_STACK_TRANSITION_TYPE_SLIDE_LEFT)fail("RTL backward direction");break;
 default:g_print("BOOKMARKS_GTK_PASS native slides, outgoing content, cleanup, search, edit, RTL and reduced motion\n");g_action_group_activate_action(G_ACTION_GROUP(g_application_get_default()),"quit",nullptr);return G_SOURCE_REMOVE;
 }
 return G_SOURCE_CONTINUE;
}
static gboolean start(gpointer){
 auto*a=g_application_get_default();if(!GTK_IS_APPLICATION(a))return G_SOURCE_CONTINUE;auto*w=gtk_application_get_active_window(GTK_APPLICATION(a));if(!w)return G_SOURCE_CONTINUE;
 auto*d=ADW_IS_APPLICATION_WINDOW(w)?adw_application_window_get_visible_dialog(ADW_APPLICATION_WINDOW(w)):nullptr;if(d&&ADW_IS_ALERT_DIALOG(d)){g_signal_emit_by_name(d,"response","no");adw_dialog_close(d);return G_SOURCE_CONTINUE;}
 sidebar=find(GTK_WIDGET(w),"EphyBookmarksDialog");if(!sidebar)return G_SOURCE_CONTINUE;
 adw_style_manager_set_color_scheme(adw_style_manager_get_default(),ADW_COLOR_SCHEME_FORCE_DARK);
 g_object_set(gtk_settings_get_default(),"gtk-enable-animations",TRUE,nullptr);
 auto get=reinterpret_cast<EphyBookmarksManager*(*)(void*)>(dlsym(RTLD_DEFAULT,"ephy_shell_get_bookmarks_manager"));auto*m=get(a);
 ephy_bookmarks_manager_create_tag(m,"Research");
 const char*names[]={"Trainsworld","City Rail Maps","GNOME Developer Documentation","A long bookmark title that should stay readable in this narrow sidebar"};const char*tags[]={"Favorites","Favorites","Research",nullptr};const char*urls[]={"https://trainsworld.example/routes","https://rail.example/maps","https://docs.gtk.org/","https://example.org/a-long-page"};
 for(unsigned i=0;i<4;i++){g_autofree char*id=g_strdup_printf("bookmark-fixture-%u",i);auto*b=ephy_bookmark_new(urls[i],names[i],nullptr,id);if(tags[i])ephy_bookmark_add_tag(b,tags[i]);ephy_bookmarks_manager_add_bookmark(m,b);g_object_unref(b);}
 stack=GTK_STACK(child("toplevel_stack"));details=GTK_LIST_BOX(child("tag_detail_list_box"));root_list=GTK_LIST_BOX(child("bookmarks_list_box"));search=GTK_SEARCH_ENTRY(child("search_entry"));
 reinterpret_cast<void(*)(void*)>(dlsym(RTLD_DEFAULT,"ephy_window_toggle_bookmarks"))(w);
 due=g_get_monotonic_time()+600000;g_timeout_add(15,tick,nullptr);return G_SOURCE_REMOVE;
}
__attribute__((constructor))static void init(){char exe[2048];auto n=readlink("/proc/self/exe",exe,sizeof(exe)-1);if(n<0)return;exe[n]=0;if(strstr(exe,"/.gnosis-wrapped"))g_timeout_add_seconds(2,start,nullptr);}
