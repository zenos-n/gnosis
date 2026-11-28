/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "browser.hpp"
#include "display-state.h"
#include <adwaita.h>
#include <dlfcn.h>
#include <unistd.h>
#include <cstring>
#include <vector>
#include <algorithm>
#include <cmath>
static GtkWindow *window;
static GtkWidget *host;
static std::shared_ptr<GnosisBrave::Page> page;
extern "C" { int gnosis_hdr_fixture_mode = -1; extern int gnosis_test_variable_feedback, gnosis_test_unknown_feedback, gnosis_test_outer_feedback, gnosis_test_target_metadata; }
#define fixture gnosis_hdr_fixture_mode
static int stage, fullscreen_cycles, window_cycles, f11_events;
static bool window_states_done;
static std::string remote;
static GDBusConnection *bus;
static bool busy;
static void fail(const char *message);
static void f11() {
  gtk_window_present(window);
  gtk_widget_grab_focus(host);
  page->focus();
  static bool focused=false;
  if(!focused) {
    for(auto event:{0,1,2}) {
      g_autoptr(GError) error=nullptr;
      auto *reply=g_dbus_connection_call_sync(bus,"org.gnome.Mutter.RemoteDesktop",remote.c_str(),
        "org.gnome.Mutter.RemoteDesktop.Session",event==0?"NotifyPointerMotionRelative":"NotifyPointerButton",
        event==0?g_variant_new("(dd)",100.,100.):g_variant_new("(ib)",272,event==1),
        nullptr,G_DBUS_CALL_FLAGS_NONE,3000,nullptr,&error);
      if(!reply)fail(error?error->message:"private focus click");g_variant_unref(reply);
    }
    focused=true;
  }
  for(bool pressed:{true,false}) {
    g_autoptr(GError) error=nullptr;
    auto *reply=g_dbus_connection_call_sync(bus,"org.gnome.Mutter.RemoteDesktop",remote.c_str(),
      "org.gnome.Mutter.RemoteDesktop.Session","NotifyKeyboardKeycode",g_variant_new("(ub)",87,pressed),
      nullptr,G_DBUS_CALL_FLAGS_NONE,3000,nullptr,&error);
    if(!reply)fail(error?error->message:"private F11 injection failed");
    g_variant_unref(reply);
  }
}
static void change_scale() {
  busy = true;
  g_dbus_connection_call(bus, "org.gnome.Mutter.DisplayConfig", "/org/gnome/Mutter/DisplayConfig",
    "org.gnome.Mutter.DisplayConfig", "GetCurrentState", nullptr, nullptr, G_DBUS_CALL_FLAGS_NONE, 5000, nullptr,
    +[](GObject *source, GAsyncResult *result, gpointer) {
      GError *error = nullptr;
      auto *state = g_dbus_connection_call_finish(G_DBUS_CONNECTION(source), result, &error);
      if (!state) fail(error->message);
      auto *serial = g_variant_get_child_value(state, 0);
      auto *monitors = g_variant_get_child_value(state, 1);
      if (g_variant_n_children(monitors) != 1) fail("expected one isolated virtual monitor");
      auto *monitor = g_variant_get_child_value(monitors, 0);
      auto *spec = g_variant_get_child_value(monitor, 0);
      auto *modes = g_variant_get_child_value(monitor, 1);
      auto *mode = g_variant_get_child_value(modes, 0);
      const char *connector, *mode_id;
      g_variant_get_child(spec, 0, "&s", &connector); g_variant_get_child(mode, 0, "&s", &mode_id);
      if (!g_str_has_prefix(connector, "Meta-")) fail("refusing to configure a physical monitor");
      GVariantBuilder physical, logical, properties;
      g_variant_builder_init(&physical, G_VARIANT_TYPE("a(ssa{sv})"));
      g_variant_builder_add(&physical, "(ss@a{sv})", connector, mode_id,
        g_variant_new_array(G_VARIANT_TYPE("{sv}"), nullptr, 0));
      g_variant_builder_init(&logical, G_VARIANT_TYPE("a(iiduba(ssa{sv}))"));
      g_variant_builder_add(&logical, "(iidub@a(ssa{sv}))", 0, 0, g_ascii_strtod(g_getenv("GNOSIS_HDR_TEST_SCALE"), nullptr), 0u, TRUE, g_variant_builder_end(&physical));
      g_variant_builder_init(&properties, G_VARIANT_TYPE("a{sv}"));
      g_variant_builder_add(&properties, "{sv}", "layout-mode", g_variant_new_uint32(1));
      g_dbus_connection_call(bus, "org.gnome.Mutter.DisplayConfig", "/org/gnome/Mutter/DisplayConfig",
        "org.gnome.Mutter.DisplayConfig", "ApplyMonitorsConfig",
        g_variant_new("(uu@a(iiduba(ssa{sv}))@a{sv})", g_variant_get_uint32(serial), 1u,
          g_variant_builder_end(&logical), g_variant_builder_end(&properties)), nullptr,
        G_DBUS_CALL_FLAGS_NONE, 5000, nullptr, +[](GObject *source, GAsyncResult *result, gpointer) {
          GError *error = nullptr;
          auto *reply = g_dbus_connection_call_finish(G_DBUS_CONNECTION(source), result, &error);
          if (!reply) fail(error->message);
          g_variant_unref(reply); busy = false;
        }, nullptr);
      g_variant_unref(mode); g_variant_unref(modes); g_variant_unref(spec);
      g_variant_unref(monitor); g_variant_unref(monitors); g_variant_unref(serial); g_variant_unref(state);
    }, nullptr);
}

static gint64 started, changed;
static void fail(const char *message) {g_printerr("HDR_GTK_FAIL stage=%d %s\n",stage,message);_exit(77);}
/* Fixture injection is confined to this preload: production always reads the
 * outer compositor. This exercises real nested protocol/Chromium negotiation. */
static void release_fixture_color(gpointer state) {gdk_color_state_unref(static_cast<GdkColorState *>(state));}
extern "C" GdkColorState *gnosis_gdk_wayland_surface_get_preferred(GdkSurface *surface, guint32 values[5]) {
  if (fixture >= 0) {
    guint32 sample[] = {0, fixture ? 10000u : 80u, fixture ? 267u : 80u, 0, fixture ? 1000u : 80u};
    memcpy(values,sample,sizeof(sample));
    g_object_set_data_full(G_OBJECT(surface),"gnosis-preferred-color-state",
      gdk_color_state_ref(fixture?gdk_color_state_get_rec2100_pq():gdk_color_state_get_srgb()),
      release_fixture_color);
    return fixture ? gdk_color_state_get_rec2100_pq() : gdk_color_state_get_srgb();
  }
  return reinterpret_cast<GdkColorState *(*)(GdkSurface *,guint32 *)>(dlsym(RTLD_NEXT,"gnosis_gdk_wayland_surface_get_preferred"))(surface,values);
}
extern "C" gboolean gnosis_gdk_wayland_surface_get_target(GdkSurface *surface, gint32 primaries[8], guint32 light[2]) {
  if(fixture>=0){const gint32 sample[]={680000,320000,265000,690000,150000,60000,312700,329000};memcpy(primaries,sample,sizeof(sample));light[0]=1000;light[1]=400;return fixture==1;}
  return reinterpret_cast<gboolean(*)(GdkSurface*,gint32*,guint32*)>(dlsym(RTLD_NEXT,"gnosis_gdk_wayland_surface_get_target"))(surface,primaries,light);
}
static GtkWidget *find(GtkWidget *widget) {
  if (!strcmp(G_OBJECT_TYPE_NAME(widget),"GnosisCompositorView")) return widget;
  for(auto *child=gtk_widget_get_first_child(widget);child;child=gtk_widget_get_next_sibling(child))if(auto *found=find(child))return found;
  return nullptr;
}
static void evaluate(const std::string &js,std::function<void(JsonObject *)> done) {
  busy=true;page->command("Runtime.evaluate","{\"expression\":"+GnosisBrave::quote(js)+",\"returnByValue\":true,\"awaitPromise\":true,\"userGesture\":true}",[done](JsonObject *reply,const std::string &error){busy=false;if(!reply||!error.empty()||json_object_has_member(reply,"exceptionDetails"))fail("fixture JS");done(json_object_get_object_member(reply,"result"));});
}
static GdkTexture *hdr_texture(GskRenderNode *node, bool any=false) {
  auto type=gsk_render_node_get_node_type(node);
  GdkTexture *texture=type==GSK_TEXTURE_NODE?gsk_texture_node_get_texture(node):
    type==GSK_TEXTURE_SCALE_NODE?gsk_texture_scale_node_get_texture(node):nullptr;
  if(texture && (any ? gdk_texture_get_width(texture)>256 && gdk_texture_get_height(texture)>256 :
      gdk_color_state_equivalent(gdk_texture_get_color_state(texture),gdk_color_state_get_rec2100_pq())))
    return GDK_TEXTURE(g_object_ref(texture));
  gsize count=0;auto **children=gsk_render_node_get_children(node,&count);
  for(gsize i=0;i<count;++i)if(auto *child=hdr_texture(children[i],any))return child;
  return nullptr;
}
static bool check_root_color(bool hdr) {
  auto *outer=gtk_native_get_surface(GTK_NATIVE(window));
  bool correct=bool(g_object_get_data(G_OBJECT(outer),"gnosis-content-hdr"))==hdr;
  if(!correct)g_print("HDR_GTK_ROOT expected=%s actual=other\n",hdr?"PQ":"sRGB");
  return correct;
}
static bool check_sdr_ramp(bool require_srgb=true) {
  auto *snapshot=gtk_snapshot_new();
  /* Read the current nested surfaces, not a paintable cached across color changes. */
  GTK_WIDGET_GET_CLASS(host)->snapshot(host,snapshot);
  auto *node=gtk_snapshot_free_to_node(snapshot);
  if(!node)return false;
  auto *texture=hdr_texture(node,true);gsk_render_node_unref(node);if(!texture)return false;
  bool srgb=gdk_color_state_equivalent(gdk_texture_get_color_state(texture),gdk_color_state_get_srgb());
  if(require_srgb && !check_root_color(false)){g_object_unref(texture);return false;}
  if(require_srgb && !srgb){g_print("HDR_SDR_SURFACE expected=sRGB actual=other fixture=%d\n",fixture);g_object_unref(texture);return false;}
  int width=gdk_texture_get_width(texture),height=gdk_texture_get_height(texture);
  std::vector<guchar> pixels(size_t(width)*height*4);
  auto *download=gdk_texture_downloader_new(texture);
  gdk_texture_downloader_set_color_state(download,gdk_color_state_get_srgb());
  gdk_texture_downloader_set_format(download,GDK_MEMORY_R8G8B8A8);
  gdk_texture_downloader_download_into(download,pixels.data(),width*4);
  gdk_texture_downloader_free(download);g_object_unref(texture);
  size_t counts[5]={};
  for(size_t i=0;i<pixels.size();i+=4)if(pixels[i+3]==255 && abs(int(pixels[i])-pixels[i+1])<=2 && abs(int(pixels[i])-pixels[i+2])<=2)
    for(int j=0;j<5;++j) {
      double encoded=(j==4?255:64*j)/255.;
      double linear=encoded<=.04045?encoded/12.92:std::pow((encoded+.055)/1.055,2.4);
      double expected=linear<=.0031308?linear*12.92:1.055*std::pow(linear,1/2.4)-.055;
      if(abs(int(pixels[i])-int(std::lround(std::min(1.,expected)*255)))<=3)++counts[j];
    }
  g_print("HDR_SDR_RAMP fixture=%d counts=%zu,%zu,%zu,%zu,%zu\n",fixture,counts[0],counts[1],counts[2],counts[3],counts[4]);
  for(auto count:counts)if(count<size_t(width)*height/100)return false;
  return true;
}
static bool check_p3_pixels() {
  auto *snapshot=gtk_snapshot_new();GTK_WIDGET_GET_CLASS(host)->snapshot(host,snapshot);
  auto *node=gtk_snapshot_free_to_node(snapshot);if(!node)return false;
  auto *texture=hdr_texture(node,true);gsk_render_node_unref(node);if(!texture)return false;
  auto *params=gdk_cicp_params_new();gdk_cicp_params_set_color_primaries(params,12);
  gdk_cicp_params_set_transfer_function(params,13);gdk_cicp_params_set_matrix_coefficients(params,0);
  gdk_cicp_params_set_range(params,GDK_CICP_RANGE_FULL);
  auto *p3=gdk_cicp_params_build_color_state(params,nullptr);g_object_unref(params);
  if(!p3)fail("GTK cannot construct the Display P3 reference state");
  auto *outer = gtk_native_get_surface(GTK_NATIVE(window));
  auto *root_color = static_cast<GdkColorState *>(g_object_get_data(G_OBJECT(outer),"gnosis-content-color-state"));
  if(!root_color || !gdk_color_state_equivalent(root_color,p3) ||
     !gdk_color_state_equivalent(gdk_texture_get_color_state(texture),p3)) {
    gdk_color_state_unref(p3);g_object_unref(texture);return false;
  }
  int width=gdk_texture_get_width(texture),height=gdk_texture_get_height(texture);
  std::vector<guchar> pixels(size_t(width)*height*4);
  auto *download=gdk_texture_downloader_new(texture);gdk_texture_downloader_set_color_state(download,p3);
  gdk_texture_downloader_set_format(download,GDK_MEMORY_R8G8B8A8);
  gdk_texture_downloader_download_into(download,pixels.data(),width*4);
  gdk_texture_downloader_free(download);gdk_color_state_unref(p3);g_object_unref(texture);
  size_t red=0;for(size_t i=0;i<pixels.size();i+=4)if(pixels[i]>250&&pixels[i+1]<3&&pixels[i+2]<3&&pixels[i+3]==255)++red;
  g_print("SDR_P3_PIXELS %zu\n",red);return red>10000;
}
static bool check_hdr_pixels(bool video = false) {
  auto *paintable=gtk_widget_paintable_new(GTK_WIDGET(window));auto *snapshot=gtk_snapshot_new();
  gdk_paintable_snapshot(paintable,GDK_SNAPSHOT(snapshot),gtk_widget_get_width(GTK_WIDGET(window)),gtk_widget_get_height(GTK_WIDGET(window)));
  auto *node=gtk_snapshot_free_to_node(snapshot);if(!node){g_object_unref(paintable);return false;}
  auto *texture=hdr_texture(node);gsk_render_node_unref(node);g_object_unref(paintable);
  if(!texture)return false;
  int width=gdk_texture_get_width(texture),height=gdk_texture_get_height(texture);
  if(width<=0||height<=0||guint64(width)*height>16*1024*1024)fail("fixture texture dimensions");
  auto *formats=G_ENUM_CLASS(g_type_class_ref(GDK_TYPE_MEMORY_FORMAT));
  auto *format=g_enum_get_value(formats,gdk_texture_get_format(texture));
  g_print("HDR_GTK_TEXTURE %dx%d %s\n",width,height,format?format->value_nick:"unknown");
  g_type_class_unref(formats);
  auto *downloader=gdk_texture_downloader_new(texture);
  gdk_texture_downloader_set_color_state(downloader,gdk_color_state_get_rec2100_pq());
  gdk_texture_downloader_set_format(downloader,GDK_MEMORY_R16G16B16A16_FLOAT_PREMULTIPLIED);
  std::vector<uint16_t> pixels(size_t(width)*height*4);
  gdk_texture_downloader_download_into(downloader,reinterpret_cast<guchar *>(pixels.data()),width*4*sizeof(uint16_t));
  uint16_t red_bits=0,green_bits=0;
  for(size_t i=0;i<pixels.size();i+=4){if(pixels[i]<0x7c00)red_bits=std::max(red_bits,pixels[i]);if(pixels[i+1]<0x7c00)green_bits=std::max(green_bits,pixels[i+1]);}
  auto luminance=[](uint16_t bits){int exp=(bits>>10)&31;float mant=bits&1023;
    double encoded=exp?std::ldexp(1+mant/1024,exp-15):std::ldexp(mant/1024,-14);
    double power=std::pow(encoded,32./2523);
    return 10000*std::pow(std::max(power-3424./4096,0.)/(2413./128-2392./128*power),16384./2610)/203;
  };
  float peak=luminance(red_bits),green=luminance(green_bits);
  gdk_texture_downloader_free(downloader);g_object_unref(texture);
  g_print("HDR_GTK_LINEAR_PEAK %.3f\n",peak);return peak >= 2 && (!video || green < 2);
}
static gboolean tick(gpointer) {
  auto now=g_get_monotonic_time();if(now-started>90*G_USEC_PER_SEC)fail("timeout");
  if(busy||page->loading||page->session.empty()||now-changed<700000)return G_SOURCE_CONTINUE;
  auto *dialog=adw_application_window_get_visible_dialog(ADW_APPLICATION_WINDOW(window));if(dialog){adw_dialog_force_close(dialog);return G_SOURCE_CONTINUE;}
  if(stage==0) {
    if(std::abs(gdk_surface_get_scale(gtk_native_get_surface(GTK_NATIVE(window)))-g_ascii_strtod(g_getenv("GNOSIS_HDR_TEST_SCALE"),nullptr))>.001)return G_SOURCE_CONTINUE;guint32 lum[5];
    auto *outer=gtk_native_get_surface(GTK_NATIVE(window));
    auto real=reinterpret_cast<GdkColorState *(*)(GdkSurface *,guint32 *)>(dlsym(RTLD_NEXT,"gnosis_gdk_wayland_surface_get_preferred"));
    real(outer,lum);if(!lum[1] || !lum[2])fail("outer display luminance metadata was discarded");g_print("HDR_METADATA_PERSISTENCE %u %u %u %u %u\n",lum[0],lum[1],lum[2],lum[3],lum[4]);
    fixture=0;gtk_widget_queue_draw(host);stage=1;changed=now;
  } else if(stage==1) {
    evaluate("JSON.stringify({hdr:matchMedia('(dynamic-range: high)').matches,hz:1000/(frameDeltas.slice(-60).sort((a,b)=>a-b)[30]||0)})",[](JsonObject *v){auto *json=json_object_get_string_member_with_default(v,"value","");g_print("HDR_GTK_SDR_PACING %s\n",json);if(g_getenv("GNOSIS_HDR_REAL_FEEDBACK")){const char *rate=strstr(json,"\"hz\":");if(!rate||g_ascii_strtod(rate+5,nullptr)<90)fail("native120Hz display fell below90fps");}if(strstr(json,"\"hdr\":true"))fail("SDR output falsely advertised HDR");if(!check_sdr_ramp()){if(g_get_monotonic_time()-changed>5*G_USEC_PER_SEC)fail("SDR grayscale changed");return;}g_print("HDR_GTK_SDR_PASS\n");fixture=1;gtk_widget_queue_draw(host);stage=2;changed=g_get_monotonic_time();});
  } else if(stage==2) {
    evaluate("JSON.stringify({hdr:matchMedia('(dynamic-range: high)').matches,p3:matchMedia('(color-gamut: p3)').matches,rec2020:matchMedia('(color-gamut: rec2020)').matches,webgpu:!!navigator.gpu,canvas:(()=>{const c=document.createElement('canvas').getContext('2d',{colorType:'float16',colorSpace:'display-p3',toneMapping:{mode:'extended'}});const a=c.getContextAttributes();return a.colorType==='float16'&&a.toneMapping?.mode==='extended'})()})",[](JsonObject *v){
      auto *value=json_object_get_string_member_with_default(v,"value","");g_print("HDR_GTK_WEB_CAPABILITIES %s\n",value);
      if(!strstr(value,"\"hdr\":true"))return;
      if(!strstr(value,"\"canvas\":true"))fail("HDR canvas API unavailable");
      if(!check_sdr_ramp()){if(g_get_monotonic_time()-changed>5*G_USEC_PER_SEC)fail("HDR output altered SDR grayscale");return;}g_print("HDR_GTK_CAPABILITY_PASS display metadata reaches Chromium\n");
      if(!window_states_done){gtk_window_maximize(window);stage=20;changed=g_get_monotonic_time();return;}
      evaluate("document.body.innerHTML='<canvas width=400 height=400></canvas>';const c=document.querySelector('canvas').getContext('2d',{colorSpace:'display-p3'});c.fillStyle='color(display-p3 1 0 0)';c.fillRect(0,0,400,400)",[](JsonObject *){stage=27;changed=g_get_monotonic_time();});
    });
  } else if(stage>=20 && stage<=25) {
    bool ready=stage==20?gtk_window_is_maximized(window):
      stage==21||stage==24?gtk_window_is_fullscreen(window):
      stage==22?!gtk_window_is_fullscreen(window)&&gtk_window_is_maximized(window):
      !gtk_window_is_fullscreen(window)&&!gtk_window_is_maximized(window);
    if(!ready)return G_SOURCE_CONTINUE;
    if(!check_sdr_ramp()){if(now-changed>5*G_USEC_PER_SEC)fail("window state changed SDR pixels");return G_SOURCE_CONTINUE;}
    g_print("HDR_WINDOW_MODE_PASS stage=%d cycle=%d maximized=%d fullscreen=%d size=%dx%d\n",stage,window_cycles,
      gtk_window_is_maximized(window),gtk_window_is_fullscreen(window),gtk_widget_get_width(host),gtk_widget_get_height(host));
    if(stage==22)gtk_window_unmaximize(window);
    else if(stage==25){
      if(++window_cycles==3){window_states_done=true;stage=2;changed=now;return G_SOURCE_CONTINUE;}
      gtk_window_maximize(window);stage=20;changed=now;return G_SOURCE_CONTINUE;
    } else f11();
    ++stage;changed=now;
  } else if(stage==27) {
    if(!check_p3_pixels()){if(now-changed>5*G_USEC_PER_SEC)fail("Display P3 SDR was clipped or mislabeled");return G_SOURCE_CONTINUE;}
    g_print("SDR_P3_PASS native P3 surface retains out-of-sRGB red\n");
    evaluate("document.body.innerHTML='<img src=\"/hdr.png\" width=400 height=400>'",[](JsonObject *){stage=3;changed=g_get_monotonic_time();});
  } else if(stage==3) {
    if(!check_hdr_pixels()){changed=now;return G_SOURCE_CONTINUE;}
    if(!check_sdr_ramp(false))fail("HDR image changed SDR document grays");
    if(!check_root_color(true)){if(now-changed>5*G_USEC_PER_SEC)fail("HDR rounded-edge fallback is still SDR");return G_SOURCE_CONTINUE;}
    g_print("HDR_GTK_WINDOWED_PASS native center and rounded-edge fallback both use HDR\n");
    evaluate("document.body.innerHTML='<video src=\"/hdr.webm\" width=400 height=400 autoplay muted loop></video>';document.querySelector('video').muted=true;document.querySelector('video').play()",[](JsonObject *){stage=7;changed=g_get_monotonic_time();});
  } else if(stage==7) {
    evaluate("document.querySelector('video').currentTime>0.5 && document.querySelector('video').getVideoPlaybackQuality().totalVideoFrames>3",[](JsonObject *v){
      if(!json_object_get_boolean_member_with_default(v,"value",false)||!check_hdr_pixels(true))return;
      if(!check_sdr_ramp(false))fail("HDR video changed SDR document grays");
      g_print("HDR_GTK_VIDEO_PASS VP9 10-bit PQ retains red highlight and SDR green\n");evaluate("document.querySelector('video').requestFullscreen()",[](JsonObject *){stage=6;changed=g_get_monotonic_time();});
    });
  } else if(stage==6) {
    if(!gtk_window_is_fullscreen(window) || gtk_widget_get_width(host)<2000)return G_SOURCE_CONTINUE;
    if(!check_hdr_pixels(true)){changed=now;return G_SOURCE_CONTINUE;}
    g_print("HDR_GTK_FULLSCREEN_PASS actual HDR texture retains highlights\n");
    evaluate("document.exitFullscreen()",[](JsonObject *){stage=8;changed=g_get_monotonic_time();});
  } else if(stage==8) {
    if(gtk_window_is_fullscreen(window) || gtk_widget_get_width(host)>2000)return G_SOURCE_CONTINUE;
    if(!check_hdr_pixels(true))return G_SOURCE_CONTINUE;
    g_print("HDR_GTK_FULLSCREEN_EXIT_PASS cycle=%d\n",++fullscreen_cycles);
    if(fullscreen_cycles<3)
      evaluate("document.querySelector('video').requestFullscreen()",[](JsonObject *){stage=6;changed=g_get_monotonic_time();});
    else {fixture=0;gtk_widget_queue_draw(host);stage=4;changed=now;}
  } else if(stage==4) {
    evaluate("!matchMedia('(dynamic-range: high)').matches",[](JsonObject *v){if(!json_object_get_boolean_member_with_default(v,"value",false))return;
      evaluate("document.body.innerHTML='<p>SDR again</p>'",[](JsonObject *){stage=5;changed=g_get_monotonic_time();});});
  } else if(stage==5) {
    if(!check_root_color(false)){if(now-changed>5*G_USEC_PER_SEC)fail("root remained HDR after SDR return");return G_SOURCE_CONTINUE;}
    if(f11_events!=12)fail("missing real F11 key events");
    if (!gnosis_test_outer_feedback || !gnosis_test_target_metadata || (!g_getenv("GNOSIS_HDR_REAL_FEEDBACK") && (!gnosis_test_variable_feedback || !gnosis_test_unknown_feedback))) fail("missing actual/variable/unknown presentation feedback");
    g_print("HDR_GTK_TARGET_PASS gamut and MaxCLL/MaxFALL reach nested surfaces\n");
    g_print("HDR_GTK_PRESENTATION_PASS real outer callbacks=%d, variable=%d, unknown=%d\n",gnosis_test_outer_feedback,gnosis_test_variable_feedback,gnosis_test_unknown_feedback);
    g_print("HDR_GTK_PASS SDR→HDR→SDR negotiation and content-only HDR window rendering\n");page.reset();g_action_group_activate_action(G_ACTION_GROUP(g_application_get_default()),"quit",nullptr);return G_SOURCE_REMOVE;
  }
  return G_SOURCE_CONTINUE;
}
static gboolean start(gpointer) {
  auto *app=g_application_get_default();if(!app)return G_SOURCE_CONTINUE;
  window=gtk_application_get_active_window(GTK_APPLICATION(app));if(!window)return G_SOURCE_CONTINUE;
  auto embed=reinterpret_cast<void *(*)(void *)>(dlsym(RTLD_DEFAULT,"ephy_window_get_active_embed"))(window);if(!embed)return G_SOURCE_CONTINUE;
  auto view=reinterpret_cast<GtkWidget *(*)(void *)>(dlsym(RTLD_DEFAULT,"ephy_embed_get_web_view"))(embed);host=find(view);if(!host)return G_SOURCE_CONTINUE;
  page=reinterpret_cast<std::shared_ptr<GnosisBrave::Page>(*)(void *)>(dlsym(RTLD_DEFAULT,"_Z27ephy_engine_view_brave_pageP15_EphyEngineView"))(view);
  GnosisBrave::browser_command(true,"SystemInfo.getInfo","{}",[](JsonObject *reply,const std::string &error){
    if(!reply||!error.empty())return;auto *gpu=json_object_get_object_member(reply,"gpu");if(gpu)if(auto *features=json_object_get_object_member(gpu,"featureStatus"))for(auto *key:{"gpu_compositing","skia_graphite","vulkan"})g_printerr("HDR_GTK_GPU %s=%s\n",key,json_object_get_string_member_with_default(features,key,"unknown"));
  });
  auto *events=gtk_event_controller_legacy_new();
  gtk_event_controller_set_propagation_phase(events,GTK_PHASE_CAPTURE);
  g_signal_connect(events,"event",G_CALLBACK(+[](GtkEventControllerLegacy *,GdkEvent *e,gpointer)->gboolean {
    if(gdk_event_get_event_type(e)==GDK_KEY_PRESS && gdk_key_event_get_keyval(e)==GDK_KEY_F11){++f11_events;g_print("HDR_NATIVE_KEY F11\n");}
    return false;
  }),nullptr);gtk_widget_add_controller(GTK_WIDGET(window),events);
  bus=g_bus_get_sync(G_BUS_TYPE_SESSION,nullptr,nullptr);if(!bus)fail("private bus unavailable");
  auto *monitor=gdk_display_get_monitor_at_surface(gdk_display_get_default(),gtk_native_get_surface(GTK_NATIVE(window)));
  if(!monitor||!g_str_has_prefix(gdk_monitor_get_connector(monitor),"Meta-"))fail("refusing input on physical display");
  g_autoptr(GError) error=nullptr;
  auto *reply=g_dbus_connection_call_sync(bus,"org.gnome.Mutter.RemoteDesktop","/org/gnome/Mutter/RemoteDesktop",
    "org.gnome.Mutter.RemoteDesktop","CreateSession",nullptr,nullptr,G_DBUS_CALL_FLAGS_NONE,3000,nullptr,&error);
  if(!reply)fail(error?error->message:"private input session");
  const char *path;g_variant_get(reply,"(&o)",&path);remote=path;g_variant_unref(reply);
  reply=g_dbus_connection_call_sync(bus,"org.gnome.Mutter.RemoteDesktop",remote.c_str(),
    "org.gnome.Mutter.RemoteDesktop.Session","Start",nullptr,nullptr,G_DBUS_CALL_FLAGS_NONE,3000,nullptr,&error);
  if(!reply)fail(error?error->message:"private input start");g_variant_unref(reply);
  for(bool pressed:{true,false}) {
    reply=g_dbus_connection_call_sync(bus,"org.gnome.Mutter.RemoteDesktop",remote.c_str(),
      "org.gnome.Mutter.RemoteDesktop.Session","NotifyKeyboardKeycode",g_variant_new("(ub)",42,pressed),
      nullptr,G_DBUS_CALL_FLAGS_NONE,3000,nullptr,&error);
    if(!reply)fail(error?error->message:"prime private keyboard");g_variant_unref(reply);
  }
  change_scale();started=changed=g_get_monotonic_time();g_timeout_add(30,tick,nullptr);return G_SOURCE_REMOVE;
}
__attribute__((constructor))static void setup(){char exe[2048];auto n=readlink("/proc/self/exe",exe,sizeof(exe)-1);if(n<0)return;exe[n]=0;if(strstr(exe,"/.gnosis-wrapped"))g_timeout_add_seconds(3,start,nullptr);}
