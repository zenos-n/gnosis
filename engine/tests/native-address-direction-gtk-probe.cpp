/* SPDX-License-Identifier: GPL-3.0-or-later */
#include <string>
#include <gtk/gtk.h>
#include <dlfcn.h>
#include <unistd.h>
#include <cstring>
static GtkWidget *entry;
static GtkWindow *window;
static GtkWidget *find(GtkWidget *w,const char *type) {if(!strcmp(G_OBJECT_TYPE_NAME(w),type))return w;for(auto *c=gtk_widget_get_first_child(w);c;c=gtk_widget_get_next_sibling(c))if(auto *f=find(c,type))return f;return nullptr;}
static gboolean read(gpointer) {
  auto *text=find(entry,"GtkText");graphene_rect_t a,b;gtk_text_compute_cursor_extents(GTK_TEXT(text),0,&a,&b);
  g_print("ADDRESS_PROBE xalign=%.2f direction=%d width=%d textwidth=%d cursor0=%.2f string=%s\n",gtk_editable_get_alignment(GTK_EDITABLE(entry)),gtk_widget_get_direction(text),gtk_widget_get_width(entry),gtk_widget_get_width(text),a.origin.x,gtk_editable_get_text(GTK_EDITABLE(entry)));
  if(gtk_widget_get_direction(text)!=GTK_TEXT_DIR_LTR||a.origin.x>10)_exit(77);
  g_print("ADDRESS_DIRECTION_PASS URL edit resets stale RTL and starts at the left\n");
  g_action_group_activate_action(G_ACTION_GROUP(g_application_get_default()),"quit",nullptr);return G_SOURCE_REMOVE;
}
static gboolean start(gpointer) {
 auto *win=gtk_application_get_active_window(GTK_APPLICATION(g_application_get_default()));if(!win)return G_SOURCE_CONTINUE;
 auto *loc=find(GTK_WIDGET(win),"EphyLocationEntry");if(!loc)return G_SOURCE_CONTINUE;
 window=win;entry=find(loc,"GtkEntry");reinterpret_cast<void(*)(void*)>(dlsym(RTLD_DEFAULT,"ephy_window_focus_location_entry"))(win);
 std::string longurl="https://example.com/";longurl+=std::string(500,'a');gtk_editable_set_text(GTK_EDITABLE(entry),longurl.c_str());gtk_editable_set_position(GTK_EDITABLE(entry),-1);
 g_timeout_add(300,+[](gpointer)->gboolean{gtk_editable_set_text(GTK_EDITABLE(entry),"https://www.youtube.com/?feature=youtu.be");gtk_editable_select_region(GTK_EDITABLE(entry),0,-1);
 auto *text=find(entry,"GtkText");gtk_widget_activate_action(text,"misc.toggle-direction",nullptr);
 gtk_window_set_focus(window,nullptr);
 reinterpret_cast<void(*)(void*)>(dlsym(RTLD_DEFAULT,"ephy_window_focus_location_entry"))(window);
 g_timeout_add(300,read,nullptr);return G_SOURCE_REMOVE;},nullptr);
 return G_SOURCE_REMOVE;
}
__attribute__((constructor))static void setup(){char exe[2048];auto n=readlink("/proc/self/exe",exe,sizeof(exe)-1);if(n<0)return;exe[n]=0;if(strstr(exe,"/.gnosis-wrapped"))g_timeout_add_seconds(3,start,nullptr);}
