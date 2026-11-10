/* SPDX-License-Identifier: GPL-3.0-or-later
 * Real GTK shortcut input and chooser, with disposable browser editors. */
#include "browser.hpp"
#include <adwaita.h>
#include <gdk/x11/gdkx.h>
#include <dlfcn.h>
#include <unistd.h>
#include <cstring>
#include <vector>

extern "C" GSubprocess* g_subprocess_launcher_spawnv(GSubprocessLauncher* l,const gchar*const* argv,GError** error) {
  auto real=reinterpret_cast<GSubprocess*(*)(GSubprocessLauncher*,const gchar*const*,GError**)>(dlsym(RTLD_NEXT,"g_subprocess_launcher_spawnv"));
  if(!argv||!*argv||!strstr(*argv,"brave"))return real(l,argv,error);
  std::vector<const char*> args;for(auto*p=argv;*p;++p)args.push_back(*p);args.push_back("--site-per-process");args.push_back(nullptr);
  return real(l,args.data(),error);
}
static GtkWindow *window, *main_window;
static GtkWidget* native_entry;
static std::shared_ptr<GnosisBrave::Page> page;
static std::vector<std::function<void()>> steps;
static size_t step;
static bool busy,ready;
static gint64 started;
static void fail(const char* text){g_printerr("EMOJI_GTK_FAIL step=%zu %s\n",step,text);_exit(77);}
static GtkWidget* find(GtkWidget* node,const char* type){
  if(!strcmp(G_OBJECT_TYPE_NAME(node),type))return node;
  for(auto*c=gtk_widget_get_first_child(node);c;c=gtk_widget_get_next_sibling(c))if(auto*r=find(c,type))return r;
  return nullptr;
}
static void eval(const std::string& script,std::function<void(JsonObject*)> done={}){
  busy=true;page->command("Runtime.evaluate","{\"expression\":"+GnosisBrave::quote(script)+",\"returnByValue\":true}",[done](JsonObject*r,const std::string&e){
    busy=false;if(!r||!e.empty()||json_object_has_member(r,"exceptionDetails"))fail("evaluation");if(done)done(json_object_get_object_member(r,"result"));
  });
}
static void script(const char* code){steps.push_back([code]{eval(code);});}
static void expect(const char* code){steps.push_back([code]{eval(code,[code](JsonObject*r){if(!r||!json_object_get_boolean_member_with_default(r,"value",false))fail(code);});});}
static void key(const char* keys){steps.push_back([keys]{
  auto*tool=g_getenv("GNOSIS_XDOTOOL");if(!tool)fail("no xdotool");
  g_autofree char*id=g_strdup_printf("%lu",gdk_x11_surface_get_xid(gtk_native_get_surface(GTK_NATIVE(window))));
  char*focus[]={const_cast<char*>(tool),const_cast<char*>("windowfocus"),id,nullptr};
  char*command[]={const_cast<char*>(tool),const_cast<char*>("key"),const_cast<char*>("--clearmodifiers"),const_cast<char*>(keys),nullptr};
  int status=0;if(!g_spawn_sync(nullptr,focus,nullptr,G_SPAWN_DEFAULT,nullptr,nullptr,nullptr,nullptr,&status,nullptr)||status||
    !g_spawn_sync(nullptr,command,nullptr,G_SPAWN_DEFAULT,nullptr,nullptr,nullptr,nullptr,&status,nullptr)||status)fail("XTest key");
});}
static void picker(bool visible){steps.push_back([visible]{auto*p=find(GTK_WIDGET(window),"GtkEmojiChooser");if(bool(p&&gtk_widget_get_mapped(p))!=visible)fail(visible?"native picker missing":"unexpected picker");});}
static void pick(const char* emoji){steps.push_back([emoji]{auto*p=find(GTK_WIDGET(window),"GtkEmojiChooser");if(!p||!gtk_widget_get_mapped(p))fail("no mapped emoji chooser");g_signal_emit_by_name(p,"emoji-picked",emoji);});}
static gboolean tick(gpointer){
  if(g_get_monotonic_time()-started>90*G_USEC_PER_SEC)fail("timeout");
  if(busy||page->session.empty())return G_SOURCE_CONTINUE;
  if(!ready){eval("document.readyState==='complete'&&!!document.querySelector('#input')&&frames[0].document.readyState==='complete'",[](JsonObject*r){ready=json_object_get_boolean_member_with_default(r,"value",false);if(ready)page->focus();});return G_SOURCE_CONTINUE;}
  if(step==steps.size()){g_print("EMOJI_GTK_PASS actual Ctrl+semicolon, input/textarea/contenteditable/shadow/frames, undo, cancellation and lifetime\n");page.reset();g_action_group_activate_action(G_ACTION_GROUP(g_application_get_default()),"quit",nullptr);return G_SOURCE_REMOVE;}
  g_printerr("EMOJI_STEP %zu\n",step);steps[step++]();return G_SOURCE_CONTINUE;
}
static gboolean start(gpointer){
  auto*app=g_application_get_default();if(!GTK_IS_APPLICATION(app))return G_SOURCE_CONTINUE;
  window=gtk_application_get_active_window(GTK_APPLICATION(app));if(!window)return G_SOURCE_CONTINUE;
  if(auto*d=adw_application_window_get_visible_dialog(ADW_APPLICATION_WINDOW(window))) {adw_dialog_force_close(d);return G_SOURCE_CONTINUE;}
  auto embed=reinterpret_cast<void*(*)(void*)>(dlsym(RTLD_DEFAULT,"ephy_window_get_active_embed"));
  auto view=reinterpret_cast<void*(*)(void*)>(dlsym(RTLD_DEFAULT,"ephy_embed_get_web_view"));
  auto get=reinterpret_cast<std::shared_ptr<GnosisBrave::Page>(*)(void*)>(dlsym(RTLD_DEFAULT,"_Z27ephy_engine_view_brave_pageP15_EphyEngineView"));
  if(!embed(window))return G_SOURCE_CONTINUE;page=get(view(embed(window)));started=g_get_monotonic_time();
  script("input.focus();input.setSelectionRange(6,11)");key("ctrl+semicolon");picker(true);pick("🙂");picker(false);expect("input.value==='hello 🙂'&&document.activeElement===input");
  key("ctrl+z");expect("input.value==='hello world'");
  script("textarea.focus();textarea.setSelectionRange(2,2)");key("ctrl+semicolon");pick("👨‍👩‍👧‍👦");expect("textarea.value==='a\\n👨‍👩‍👧‍👦b'");
  script("editable.focus();let r=document.createRange();r.setStart(editable.firstChild,1);r.setEnd(editable.firstChild,3);getSelection().removeAllRanges();getSelection().addRange(r)");
  key("ctrl+semicolon");pick("❤️");expect("editable.textContent==='h❤️lo'");
  script("shadow.shadowRoot.querySelector('input').focus()");key("ctrl+semicolon");pick("🌍");expect("shadow.shadowRoot.querySelector('input').value==='🌍'");
  script("frames[0].document.querySelector('input').focus()");key("ctrl+semicolon");pick("🚆");expect("frames[0].document.querySelector('input').value==='🚆'");
  script("cross.contentWindow.postMessage(\"focus\",\"*\")");key("ctrl+semicolon");pick("🔑");expect("crossValue===\"🔑\"");
  script("input.focus();input.setSelectionRange(0,0)");key("ctrl+semicolon");picker(true);key("Escape");picker(false);expect("input.value==='hello world'");
  script("input.focus()");key("ctrl+semicolon");picker(true);script("textarea.focus()");pick("❌");expect("textarea.value==='a\\n👨‍👩‍👧‍👦b'&&input.value==='hello world'");
  script("readonly.focus()");key("ctrl+semicolon");picker(false);script("document.activeElement.blur()");key("ctrl+semicolon");picker(false);
  script("input.focus()");key("ctrl+semicolon");picker(true);steps.push_back([]{page->set_visible(false);});picker(false);steps.push_back([]{page->set_visible(true);page->focus();});
  steps.push_back([]{main_window=window;window=GTK_WINDOW(gtk_window_new());gtk_window_set_transient_for(window,main_window);native_entry=gtk_entry_new();gtk_window_set_child(window,native_entry);gtk_window_present(window);gtk_widget_grab_focus(native_entry);});
  key("ctrl+semicolon");picker(true);pick("🦉");steps.push_back([]{if(strcmp(gtk_editable_get_text(GTK_EDITABLE(native_entry)),"🦉"))fail("native GTK entry insertion");gtk_window_destroy(window);window=main_window;page->focus();});
  script("input.focus()");key("ctrl+semicolon");picker(true);steps.push_back([]{page->navigate("about:blank");});picker(false);
  g_timeout_add(350,tick,nullptr);return G_SOURCE_REMOVE;
}
__attribute__((constructor))static void init(){char exe[2048];auto n=readlink("/proc/self/exe",exe,sizeof(exe)-1);if(n<0)return;exe[n]=0;if(strstr(exe,"/.gnosis-wrapped"))g_timeout_add_seconds(2,start,nullptr);}
