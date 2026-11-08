/* SPDX-License-Identifier: GPL-3.0-or-later
 * Real keys on an isolated Mutter seat; DevTools observes the owned input only.
 */
#include "browser.hpp"
#include <adwaita.h>
#include <gdk/wayland/gdkwayland.h>
#include <xkbcommon/xkbcommon.h>
#include <dlfcn.h>
#include <unistd.h>
#include <cstring>
#include <vector>
// Configure only the owned nested Mutter; no desktop GSettings/layout is changed.
extern "C" gboolean meta_context_start(void*context,GError**error){
 auto real=reinterpret_cast<gboolean(*)(void*,GError**)>(dlsym(RTLD_NEXT,"meta_context_start"));
 if(!real(context,error))return false;
 auto backend=reinterpret_cast<void*(*)(void*)>(dlsym(RTLD_DEFAULT,"meta_context_get_backend"))(context);
 auto create=reinterpret_cast<void*(*)(const char*,const char*,const char*,const char*,char**,char**)>(dlsym(RTLD_DEFAULT,"meta_keymap_description_new_from_rules"));
 auto set=reinterpret_cast<void(*)(void*,void*,uint32_t,GCancellable*,GAsyncReadyCallback,void*)>(dlsym(RTLD_DEFAULT,"meta_backend_set_keymap_async"));
 auto unref=reinterpret_cast<void(*)(void*)>(dlsym(RTLD_DEFAULT,"meta_keymap_description_unref"));
 auto*description=create("pc105","pl,de","","grp:alt_shift_toggle",nullptr,nullptr);
 g_signal_connect_data(backend,"reset-keymap-description",G_CALLBACK(+[](void*,void*description)->void*{return reinterpret_cast<void*(*)(void*)>(dlsym(RTLD_DEFAULT,"meta_keymap_description_ref"))(description);}),description,+[](gpointer p,GClosure*){reinterpret_cast<void(*)(void*)>(dlsym(RTLD_DEFAULT,"meta_keymap_description_unref"))(p);},G_CONNECT_DEFAULT);
 reinterpret_cast<void*(*)(void*)>(dlsym(RTLD_DEFAULT,"meta_keymap_description_ref"))(description);
 set(backend,description,0,nullptr,+[](GObject*backend,GAsyncResult*result,gpointer){
   auto finish=reinterpret_cast<gboolean(*)(void*,GAsyncResult*,GError**)>(dlsym(RTLD_DEFAULT,"meta_backend_set_keymap_finish"));GError*error=nullptr;
   if(!finish(backend,result,&error))g_error("Nested test keymap: %s",error?error->message:"unknown");
   auto getmap=reinterpret_cast<xkb_keymap*(*)(void*)>(dlsym(RTLD_DEFAULT,"meta_backend_get_xkb_keymap"));auto*map=getmap(backend);
   for(unsigned i=0;i<xkb_keymap_num_layouts(map);i++)g_print("NESTED_KEYMAP %s\n",xkb_keymap_layout_get_name(map,i));
   g_autofree char*path=g_build_filename(g_get_user_runtime_dir(),"keymap-ready",nullptr);g_file_set_contents(path,"ready",-1,nullptr);
 },nullptr);unref(description);return true;
}
static GtkWindow *window;
static std::shared_ptr<GnosisBrave::Page> page;
static GDBusConnection *bus;
static std::string remote;
static std::vector<std::function<void()>> actions;
static size_t step;
static bool busy;
static void stop(){if(bus&&!remote.empty())g_dbus_connection_call_sync(bus,"org.gnome.Mutter.RemoteDesktop",remote.c_str(),"org.gnome.Mutter.RemoteDesktop.Session","Stop",nullptr,nullptr,G_DBUS_CALL_FLAGS_NONE,3000,nullptr,nullptr);remote.clear();}
static void fail(const char*s){g_printerr("KEYBOARD_LAYOUT_FAIL step=%zu %s\n",step,s);stop();_exit(77);}
static GVariant* call(const char*path,const char*iface,const char*method,GVariant*args=nullptr){GError*e=nullptr;auto*r=g_dbus_connection_call_sync(bus,"org.gnome.Mutter.RemoteDesktop",path,iface,method,args,nullptr,G_DBUS_CALL_FLAGS_NONE,5000,nullptr,&e);if(!r)fail(e?e->message:method);return r;}
static void key(int code,bool pressed){g_variant_unref(call(remote.c_str(),"org.gnome.Mutter.RemoteDesktop.Session","NotifyKeyboardKeycode",g_variant_new("(ub)",code,pressed)));}
static void press(int code){actions.push_back([=]{key(code,true);});actions.push_back([=]{key(code,false);});}
static void eval(const std::string&js){actions.push_back([js]{busy=true;page->command("Runtime.evaluate","{\"expression\":"+GnosisBrave::quote(js)+",\"returnByValue\":true}",[js](JsonObject*r,const std::string&e){busy=false;auto*v=r?json_object_get_object_member(r,"result"):nullptr;if(!e.empty()||!v||!json_object_get_boolean_member_with_default(v,"value",false)){if(v){auto*n=json_node_new(JSON_NODE_OBJECT);json_node_set_object(n,v);g_autofree char*s=json_to_string(n,false);g_printerr("KEYBOARD_LAYOUT_RESULT %s\n",s);json_node_free(n);}page->command("Runtime.evaluate",R"JSON({"expression":"JSON.stringify({value:document.querySelector('input').value,keys})","returnByValue":true})JSON",[js](JsonObject*r,const std::string&){auto*v=r?json_object_get_object_member(r,"result"):nullptr;if(v)g_printerr("INPUT_STATE %s\n",json_object_get_string_member_with_default(v,"value","unknown"));fail(js.c_str());});return;}g_print("KEYBOARD_LAYOUT_CHECK %s\n",js.c_str());});});}
static gboolean tick(gpointer){if(busy)return G_SOURCE_CONTINUE;if(step==actions.size()){stop();page.reset();g_print("KEYBOARD_LAYOUT_PASS Polish AltGr, uppercase, pointer sync, layout groups, Ctrl shortcut\n");g_action_group_activate_action(G_ACTION_GROUP(g_application_get_default()),"quit",nullptr);return G_SOURCE_REMOVE;}actions[step++]();return G_SOURCE_CONTINUE;}
static gboolean start(gpointer){
 auto*a=g_application_get_default();if(!GTK_IS_APPLICATION(a))return G_SOURCE_CONTINUE;window=gtk_application_get_active_window(GTK_APPLICATION(a));if(!window)return G_SOURCE_CONTINUE;
 auto*d=ADW_IS_APPLICATION_WINDOW(window)?adw_application_window_get_visible_dialog(ADW_APPLICATION_WINDOW(window)):nullptr;if(d&&ADW_IS_ALERT_DIALOG(d)){g_signal_emit_by_name(d,"response","no");adw_dialog_close(d);return G_SOURCE_CONTINUE;}
 auto embed=reinterpret_cast<void*(*)(void*)>(dlsym(RTLD_DEFAULT,"ephy_window_get_active_embed"));auto view=reinterpret_cast<GtkWidget*(*)(void*)>(dlsym(RTLD_DEFAULT,"ephy_embed_get_web_view"));auto getpage=reinterpret_cast<std::shared_ptr<GnosisBrave::Page>(*)(void*)>(dlsym(RTLD_DEFAULT,"_Z27ephy_engine_view_brave_pageP15_EphyEngineView"));if(!embed(window))return G_SOURCE_CONTINUE;page=getpage(view(embed(window)));if(!page||page->session.empty()||page->loading||page->uri.find("keyboard-layout.html")==std::string::npos)return G_SOURCE_CONTINUE;
 auto*device=gdk_seat_get_keyboard(gdk_display_get_default_seat(gdk_display_get_default()));auto*map=gdk_wayland_device_get_xkb_keymap(device);for(unsigned i=0;i<xkb_keymap_num_layouts(map);i++)g_print("KEYBOARD_LAYOUT_HOST %u %s\n",i,xkb_keymap_layout_get_name(map,i));
 auto*events=gtk_event_controller_legacy_new();gtk_event_controller_set_propagation_phase(events,GTK_PHASE_CAPTURE);g_signal_connect(events,"event",G_CALLBACK(+[](GtkEventControllerLegacy*,GdkEvent*e,gpointer)->gboolean{if(gdk_event_get_event_type(e)==GDK_KEY_PRESS){auto*m=gdk_wayland_device_get_xkb_keymap(gdk_event_get_device(e));g_print("OUTER_KEY %s layout%u level%u map%s\n",gdk_keyval_name(gdk_key_event_get_keyval(e)),gdk_key_event_get_layout(e),gdk_key_event_get_level(e),xkb_keymap_layout_get_name(m,0));}return false;}),nullptr);gtk_widget_add_controller(GTK_WIDGET(window),events);
 gtk_window_present(window);page->focus();bus=g_bus_get_sync(G_BUS_TYPE_SESSION,nullptr,nullptr);auto*r=call("/org/gnome/Mutter/RemoteDesktop","org.gnome.Mutter.RemoteDesktop","CreateSession");const char*path;g_variant_get(r,"(&o)",&path);remote=path;g_variant_unref(r);g_variant_unref(call(remote.c_str(),"org.gnome.Mutter.RemoteDesktop.Session","Start"));
 eval("document.querySelector('input').focus(),true");
 // The nested test seat starts with Polish and has German as its second group.
 actions.push_back([]{key(100,true);});press(38); // AltGr+L -> ł
 actions.push_back([]{key(42,true);});press(38);actions.push_back([]{key(42,false);}); // Ł
 actions.push_back([]{g_variant_unref(call(remote.c_str(),"org.gnome.Mutter.RemoteDesktop.Session","NotifyPointerAxis",g_variant_new("(ddu)",0.,1.,0)));});
 press(30);actions.push_back([]{key(100,false);}); // ą after pointer modifier synchronization
 eval("document.querySelector('input').value==='łŁą'&&keys.some(k=>k.key==='ł'&&k.trusted)");
 actions.push_back([]{key(29,true);});press(30);actions.push_back([]{key(29,false);});press(14); // native Ctrl+A, Backspace
 eval("document.querySelector('input').value===''");
 actions.push_back([]{key(56,true);});press(42);actions.push_back([]{key(56,false);}); // Alt+Shift -> German
 press(21);eval("document.querySelector('input').value==='z'");
 actions.push_back([]{key(56,true);});press(42);actions.push_back([]{key(56,false);});
 actions.push_back([]{key(100,true);});press(38);actions.push_back([]{key(100,false);});eval("document.querySelector('input').value==='zł'");
 g_timeout_add(150,tick,nullptr);return G_SOURCE_REMOVE;
}
__attribute__((constructor))static void init(){char exe[2048];auto n=readlink("/proc/self/exe",exe,sizeof(exe)-1);if(n<0)return;exe[n]=0;if(strstr(exe,"/.gnosis-wrapped"))g_timeout_add_seconds(2,start,nullptr);}
