/* SPDX-License-Identifier: GPL-3.0-or-later
 * Real GTK shortcut input and chooser, with disposable browser editors. */
#include "browser.hpp"
#include <adwaita.h>
#include <gdk/x11/gdkx.h>
#include <dlfcn.h>
#include <unistd.h>
#include <cstring>
#include <vector>
#include <cmath>

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
static gint64 started, opening;
static bool measured;
static GdkRectangle expected, last_caret, first_caret;
static bool check_anchor, exact_anchor;
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
static void anchor(const char* code,bool exact=false){steps.push_back([code,exact]{eval(code,[exact](JsonObject*r){auto*a=json_object_get_array_member(r,"value");expected={int(std::floor(json_array_get_double_element(a,0))),int(std::floor(json_array_get_double_element(a,1))),int(std::ceil(json_array_get_double_element(a,2))),int(std::ceil(json_array_get_double_element(a,3)))};check_anchor=true;exact_anchor=exact;});});}
static void verify_picker(GtkWidget*p){
 if(check_anchor){
  GdkRectangle actual;gtk_popover_get_pointing_to(GTK_POPOVER(p),&actual);last_caret=actual;
  g_printerr("EMOJI_CARET got=%d,%d,%d,%d field=%d,%d,%d,%d\n",actual.x,actual.y,actual.width,actual.height,expected.x,expected.y,expected.width,expected.height);
  if(exact_anchor){if(abs(actual.x-expected.x)>2||abs(actual.y-expected.y)>3||abs(actual.width-std::max(1,expected.width))>2||abs(actual.height-expected.height)>3)fail("native caret differs from collapsed DOM range");}
  else if(actual.width>=expected.width/2||actual.height<=0||actual.x<expected.x-2||actual.x>expected.x+expected.width+2||actual.y<expected.y-2||actual.y>expected.y+expected.height+2)fail("popover is not anchored to text caret/selection");
  check_anchor=false;
 }
}
static gboolean latency(gpointer){
 if(!opening||measured)return G_SOURCE_CONTINUE;
 auto*p=find(GTK_WIDGET(window),"GtkEmojiChooser");
 if(p&&gtk_widget_get_mapped(p)){auto us=g_get_monotonic_time()-opening;g_printerr("EMOJI_OPEN_MS %.1f\n",us/1000.);if(us>=400000)fail("emoji picker exceeded 400ms");measured=true;verify_picker(p);}
 else if(g_get_monotonic_time()-opening>=400000)fail("no visible emoji picker within 400ms");
 return G_SOURCE_CONTINUE;
}
static void key(const char* keys){steps.push_back([keys]{
  auto*tool=g_getenv("GNOSIS_XDOTOOL");if(!tool)fail("no xdotool");
  g_autofree char*id=g_strdup_printf("%lu",gdk_x11_surface_get_xid(gtk_native_get_surface(GTK_NATIVE(window))));
  char*focus[]={const_cast<char*>(tool),const_cast<char*>("windowfocus"),id,nullptr};
  char*command[]={const_cast<char*>(tool),const_cast<char*>("key"),const_cast<char*>("--clearmodifiers"),const_cast<char*>(keys),nullptr};
  if(!strcmp(keys,"ctrl+semicolon")&&check_anchor){opening=g_get_monotonic_time();measured=false;}
  int status=0;if(!g_spawn_sync(nullptr,focus,nullptr,G_SPAWN_DEFAULT,nullptr,nullptr,nullptr,nullptr,&status,nullptr)||status||
    !g_spawn_sync(nullptr,command,nullptr,G_SPAWN_DEFAULT,nullptr,nullptr,nullptr,nullptr,&status,nullptr)||status)fail("XTest key");
});}
static void picker(bool visible){steps.push_back([visible]{auto*p=find(GTK_WIDGET(window),"GtkEmojiChooser");if(bool(p&&gtk_widget_get_mapped(p))!=visible)fail(visible?"native picker missing":"unexpected picker");
 static bool captured;if(visible&&!captured&&last_caret.width<=2&&g_getenv("GNOSIS_CAPTURE_TOOL")){captured=true;char*argv[]={const_cast<char*>(g_getenv("GNOSIS_CAPTURE_TOOL")),const_cast<char*>("-window"),const_cast<char*>("root"),const_cast<char*>("/tmp/gnosis-emoji-anchor.png"),nullptr};int status=0;if(!g_spawn_sync(nullptr,argv,nullptr,G_SPAWN_DEFAULT,nullptr,nullptr,nullptr,nullptr,&status,nullptr)||status)fail("isolated capture");}});}
static void pick(const char* emoji){steps.push_back([emoji]{auto*p=find(GTK_WIDGET(window),"GtkEmojiChooser");if(!p||!gtk_widget_get_mapped(p))fail("no mapped emoji chooser");verify_picker(p);g_signal_emit_by_name(p,"emoji-picked",emoji);});}
static gboolean tick(gpointer){
  if(g_get_monotonic_time()-started>90*G_USEC_PER_SEC)fail("timeout");
  if(busy||page->session.empty())return G_SOURCE_CONTINUE;
  if(!ready){eval("document.readyState==='complete'&&!!document.querySelector('#input')&&frames[0].document.readyState==='complete'",[](JsonObject*r){ready=json_object_get_boolean_member_with_default(r,"value",false);if(ready)page->focus();});return G_SOURCE_CONTINUE;}
  if(step==steps.size()){g_print("EMOJI_GTK_PASS native caret movement, RTL, rich text, scrolled inputs, transformed frames, zoom, undo and cancellation\n");page.reset();g_action_group_activate_action(G_ACTION_GROUP(g_application_get_default()),"quit",nullptr);return G_SOURCE_REMOVE;}
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
  script("input.focus();input.setSelectionRange(6,11)");anchor("(()=>{const r=input.getBoundingClientRect();return [r.x,r.y,r.width,r.height]})()");key("ctrl+semicolon");picker(true);pick("🙂");picker(false);expect("input.value==='hello 🙂'&&document.activeElement===input");
  key("ctrl+z");expect("input.value==='hello world'");
  script("input.focus();input.setSelectionRange(0,0)");anchor("(()=>{const r=input.getBoundingClientRect();return [r.x,r.y,r.width,r.height]})()");key("ctrl+semicolon");picker(true);steps.push_back([]{first_caret=last_caret;if(last_caret.width>2)fail("collapsed caret wider than text cursor");});key("Escape");
  script("input.setSelectionRange(10,10)");anchor("(()=>{const r=input.getBoundingClientRect();return [r.x,r.y,r.width,r.height]})()");key("ctrl+semicolon");picker(true);steps.push_back([]{if(last_caret.x-first_caret.x<35||abs(last_caret.y-first_caret.y)>2||last_caret.width>2)fail("caret did not move within stationary input");});key("Escape");
  script("textarea.focus();textarea.setSelectionRange(2,2)");anchor("(()=>{const r=textarea.getBoundingClientRect();return [r.x,r.y,r.width,r.height]})()");key("ctrl+semicolon");pick("👨‍👩‍👧‍👦");expect("textarea.value==='a\\n👨‍👩‍👧‍👦b'");
  script("editable.focus();let r=document.createRange();r.setStart(editable.firstChild,1);r.setEnd(editable.firstChild,3);getSelection().removeAllRanges();getSelection().addRange(r)");
  anchor("(()=>{const r=editable.getBoundingClientRect();return [r.x,r.y,r.width,r.height]})()");key("ctrl+semicolon");pick("❤️");expect("editable.textContent==='h❤️lo'");
  script("shadow.shadowRoot.querySelector('input').focus()");anchor("(()=>{const r=shadow.shadowRoot.querySelector(\"input\").getBoundingClientRect();return [r.x,r.y,r.width,r.height]})()");key("ctrl+semicolon");pick("🌍");expect("shadow.shadowRoot.querySelector('input').value==='🌍'");
  script("frames[0].document.querySelector('input').focus()");anchor("(()=>{const f=document.querySelector(\"iframe\");const r=frames[0].document.querySelector(\"input\").getBoundingClientRect(),b=f.getBoundingClientRect();return [b.x+f.clientLeft+r.x,b.y+f.clientTop+r.y,r.width,r.height]})()");key("ctrl+semicolon");pick("🚆");expect("frames[0].document.querySelector('input').value==='🚆'");
  script("cross.contentWindow.postMessage(\"focus\",\"*\")");anchor("(()=>{const r=cross.getBoundingClientRect();return [r.x+cross.clientLeft+crossBounds[0],r.y+cross.clientTop+crossBounds[1],crossBounds[2],crossBounds[3]]})()");key("ctrl+semicolon");pick("🔑");expect("crossValue===\"🔑\"");
  steps.push_back([]{page->set_zoom(1.25);});script("input.focus();input.setSelectionRange(0,0)");anchor("(()=>{const r=input.getBoundingClientRect();return [r.x*1.25,r.y*1.25,r.width*1.25,r.height*1.25]})()");key("ctrl+semicolon");picker(true);key("Escape");steps.push_back([]{page->set_zoom(1);});
  script("input.focus();input.setSelectionRange(0,0)");key("ctrl+semicolon");picker(true);key("Escape");picker(false);expect("input.value==='hello world'");
  script("input.focus()");key("ctrl+semicolon");picker(true);script("textarea.focus()");pick("❌");expect("textarea.value==='a\\n👨‍👩‍👧‍👦b'&&input.value==='hello world'");
  script("input.dir='rtl';input.value='שלום';input.focus();input.setSelectionRange(0,0)");anchor("(()=>{const r=input.getBoundingClientRect();return [r.x,r.y,r.width,r.height]})()");key("ctrl+semicolon");picker(true);steps.push_back([]{first_caret=last_caret;});key("Escape");
  script("input.setSelectionRange(4,4)");anchor("(()=>{const r=input.getBoundingClientRect();return [r.x,r.y,r.width,r.height]})()");key("ctrl+semicolon");picker(true);steps.push_back([]{if(first_caret.x-last_caret.x<15)fail("RTL caret failed to move left");});key("Escape");script("input.dir='ltr';input.value='hello world'");
  script("editable.textContent='a long editable sentence';editable.focus();var caretRange=document.createRange();caretRange.setStart(editable.firstChild,15);caretRange.collapse(true);getSelection().removeAllRanges();getSelection().addRange(caretRange)");anchor("(()=>{const r=getSelection().getRangeAt(0).getBoundingClientRect();return [r.x,r.y,r.width,r.height]})()",true);key("ctrl+semicolon");picker(true);key("Escape");
  script("input.value='abcdefghijklmnopqrstuvwxyz'.repeat(20);input.focus()");key("End");expect("input.scrollLeft>0");anchor("(()=>{const r=input.getBoundingClientRect();return [r.x,r.y,r.width,r.height]})()");key("ctrl+semicolon");picker(true);steps.push_back([]{if(last_caret.x<expected.x+expected.width-20)fail("horizontally scrolled caret is not at visible text end");});key("Escape");script("input.value='hello world'");
  script("textarea.value=('0123456789 abcdefghijklmnopqrstuvwxyz\\n').repeat(30);textarea.focus()");key("ctrl+End");expect("textarea.scrollTop>0");anchor("(()=>{const r=textarea.getBoundingClientRect();return [r.x,r.y,r.width,r.height]})()");key("ctrl+semicolon");picker(true);steps.push_back([]{first_caret=last_caret;});key("Escape");key("Up");anchor("(()=>{const r=textarea.getBoundingClientRect();return [r.x,r.y,r.width,r.height]})()");key("ctrl+semicolon");picker(true);steps.push_back([]{if(last_caret.y>=first_caret.y-4)fail("wrapped/scrolled textarea caret failed to move up");});key("Escape");
  script("cross.style.transformOrigin='0 0';cross.style.transform='scale(.8)';cross.contentWindow.postMessage(\"focus\",\"*\")");anchor("(()=>{const r=cross.getBoundingClientRect();return [r.x+.8*(cross.clientLeft+crossBounds[0]),r.y+.8*(cross.clientTop+crossBounds[1]),crossBounds[2]*.8,crossBounds[3]*.8]})()");key("ctrl+semicolon");picker(true);key("Escape");
  script("readonly.focus()");key("ctrl+semicolon");picker(false);script("document.activeElement.blur()");key("ctrl+semicolon");picker(false);
  script("input.focus()");key("ctrl+semicolon");picker(true);steps.push_back([]{page->set_visible(false);});picker(false);steps.push_back([]{page->set_visible(true);page->focus();});
  steps.push_back([]{main_window=window;window=GTK_WINDOW(gtk_window_new());gtk_window_set_transient_for(window,main_window);native_entry=gtk_entry_new();gtk_window_set_child(window,native_entry);gtk_window_present(window);gtk_widget_grab_focus(native_entry);});
  key("ctrl+semicolon");picker(true);pick("🦉");steps.push_back([]{if(strcmp(gtk_editable_get_text(GTK_EDITABLE(native_entry)),"🦉"))fail("native GTK entry insertion");gtk_window_destroy(window);window=main_window;page->focus();});
  script("input.focus()");key("ctrl+semicolon");picker(true);steps.push_back([]{page->navigate("about:blank");});picker(false);
  g_timeout_add(5,latency,nullptr);g_timeout_add(350,tick,nullptr);return G_SOURCE_REMOVE;
}
__attribute__((constructor))static void init(){char exe[2048];auto n=readlink("/proc/self/exe",exe,sizeof(exe)-1);if(n<0)return;exe[n]=0;if(strstr(exe,"/.gnosis-wrapped"))g_timeout_add_seconds(2,start,nullptr);}
