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
static int stage;
static GDBusConnection *bus;
static bool busy;
static void fail(const char *message);
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
extern "C" GdkColorState *gnosis_gdk_wayland_surface_get_preferred(GdkSurface *surface, guint32 values[5]) {
  if (fixture >= 0) {
    guint32 sample[] = {0, fixture ? 10000u : 80u, fixture ? 203u : 80u, 0, fixture ? 1000u : 80u};
    memcpy(values,sample,sizeof(sample));
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
  switch(gsk_render_node_get_node_type(node)) {
    case GSK_TEXTURE_NODE: {
      auto *texture=gsk_texture_node_get_texture(node);
      return (any || gdk_color_state_equivalent(gdk_texture_get_color_state(texture),gdk_color_state_get_rec2100_pq()))?GDK_TEXTURE(g_object_ref(texture)):nullptr;
    }
    case GSK_TEXTURE_SCALE_NODE: {
      auto *texture=gsk_texture_scale_node_get_texture(node);
      return (any || gdk_color_state_equivalent(gdk_texture_get_color_state(texture),gdk_color_state_get_rec2100_pq()))?GDK_TEXTURE(g_object_ref(texture)):nullptr;
    }
    case GSK_CONTAINER_NODE:
      for(guint i=0;i<gsk_container_node_get_n_children(node);i++)if(auto *texture=hdr_texture(gsk_container_node_get_child(node,i),any))return texture;
      return nullptr;
    case GSK_SUBSURFACE_NODE:return hdr_texture(gsk_subsurface_node_get_child(node),any);
    case GSK_TRANSFORM_NODE:return hdr_texture(gsk_transform_node_get_child(node),any);
    case GSK_CLIP_NODE:return hdr_texture(gsk_clip_node_get_child(node),any);
    case GSK_ROUNDED_CLIP_NODE:return hdr_texture(gsk_rounded_clip_node_get_child(node),any);
    case GSK_DEBUG_NODE:return hdr_texture(gsk_debug_node_get_child(node),any);
    default:return nullptr;
  }
}
static bool check_sdr_ramp() {
  auto *paintable=gtk_widget_paintable_new(host);auto *snapshot=gtk_snapshot_new();
  gdk_paintable_snapshot(paintable,GDK_SNAPSHOT(snapshot),gtk_widget_get_width(host),gtk_widget_get_height(host));
  auto *node=gtk_snapshot_free_to_node(snapshot);g_object_unref(paintable);
  if(!node)return false;
  auto *texture=hdr_texture(node,true);gsk_render_node_unref(node);if(!texture)return false;
  int width=gdk_texture_get_width(texture),height=gdk_texture_get_height(texture);
  std::vector<guchar> pixels(size_t(width)*height*4);
  auto *download=gdk_texture_downloader_new(texture);
  gdk_texture_downloader_set_color_state(download,gdk_color_state_get_srgb());
  gdk_texture_downloader_set_format(download,GDK_MEMORY_R8G8B8A8);
  gdk_texture_downloader_download_into(download,pixels.data(),width*4);
  gdk_texture_downloader_free(download);g_object_unref(texture);
  size_t counts[5]={};
  for(size_t i=0;i<pixels.size();i+=4)if(pixels[i+3]==255 && abs(int(pixels[i])-pixels[i+1])<=2 && abs(int(pixels[i])-pixels[i+2])<=2)
    for(int j=0;j<5;++j)if(abs(int(pixels[i])-(j==4?255:64*j))<=3)++counts[j];
  g_print("HDR_SDR_RAMP fixture=%d counts=%zu,%zu,%zu,%zu,%zu\n",fixture,counts[0],counts[1],counts[2],counts[3],counts[4]);
  for(auto count:counts)if(count<100)return false;
  return true;
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
    evaluate("JSON.stringify({hdr:matchMedia('(dynamic-range: high)').matches,hz:1000/(frameDeltas.slice(-60).sort((a,b)=>a-b)[30]||0)})",[](JsonObject *v){auto *json=json_object_get_string_member_with_default(v,"value","");g_print("HDR_GTK_SDR_PACING %s\n",json);if(g_getenv("GNOSIS_HDR_REAL_FEEDBACK")){const char *rate=strstr(json,"\"hz\":");if(!rate||g_ascii_strtod(rate+5,nullptr)<90)fail("native120Hz display fell below90fps");}if(strstr(json,"\"hdr\":true"))fail("SDR output falsely advertised HDR");if(!check_sdr_ramp())fail("SDR grayscale changed");g_print("HDR_GTK_SDR_PASS\n");fixture=1;gtk_widget_queue_draw(host);stage=2;changed=g_get_monotonic_time();});
  } else if(stage==2) {
    evaluate("JSON.stringify({hdr:matchMedia('(dynamic-range: high)').matches,p3:matchMedia('(color-gamut: p3)').matches,rec2020:matchMedia('(color-gamut: rec2020)').matches,webgpu:!!navigator.gpu,canvas:(()=>{const c=document.createElement('canvas').getContext('2d',{colorType:'float16',colorSpace:'display-p3',toneMapping:{mode:'extended'}});const a=c.getContextAttributes();return a.colorType==='float16'&&a.toneMapping?.mode==='extended'})()})",[](JsonObject *v){
      auto *value=json_object_get_string_member_with_default(v,"value","");g_print("HDR_GTK_WEB_CAPABILITIES %s\n",value);
      if(!strstr(value,"\"hdr\":true"))return;
      if(!strstr(value,"\"canvas\":true"))fail("HDR canvas API unavailable");
      if(!check_sdr_ramp())fail("HDR output altered SDR grayscale");g_print("HDR_GTK_CAPABILITY_PASS display metadata reaches Chromium\n");
      evaluate("document.body.innerHTML='<img src=\"/hdr.png\" width=400 height=400>'",[](JsonObject *){stage=3;changed=g_get_monotonic_time();});
    });
  } else if(stage==3) {
    if(!check_hdr_pixels()){changed=now;return G_SOURCE_CONTINUE;}
    g_print("HDR_GTK_WINDOWED_PASS actual HDR texture retains highlights\n");
    evaluate("document.body.innerHTML='<video src=\"/hdr.webm\" width=400 height=400 autoplay muted loop></video>';document.querySelector('video').muted=true;document.querySelector('video').play()",[](JsonObject *){stage=7;changed=g_get_monotonic_time();});
  } else if(stage==7) {
    evaluate("document.querySelector('video').currentTime>0.5 && document.querySelector('video').getVideoPlaybackQuality().totalVideoFrames>3",[](JsonObject *v){
      if(!json_object_get_boolean_member_with_default(v,"value",false)||!check_hdr_pixels(true))return;
      g_print("HDR_GTK_VIDEO_PASS VP9 10-bit PQ retains red highlight and SDR green\n");evaluate("document.querySelector('video').requestFullscreen()",[](JsonObject *){stage=6;changed=g_get_monotonic_time();});
    });
  } else if(stage==6) {
    if(!gtk_window_is_fullscreen(window) || gtk_widget_get_width(host)<2000)return G_SOURCE_CONTINUE;
    if(!check_hdr_pixels(true)){changed=now;return G_SOURCE_CONTINUE;}
    g_print("HDR_GTK_FULLSCREEN_PASS actual HDR texture retains highlights\n");
    evaluate("document.exitFullscreen()",[](JsonObject *){});fixture=0;gtk_widget_queue_draw(host);stage=4;changed=now;
  } else if(stage==4) {
    evaluate("!matchMedia('(dynamic-range: high)').matches",[](JsonObject *v){if(!json_object_get_boolean_member_with_default(v,"value",false))return;
      evaluate("document.body.innerHTML='<p>SDR again</p>'",[](JsonObject *){stage=5;changed=g_get_monotonic_time();});});
  } else if(stage==5) {
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
  bus=g_bus_get_sync(G_BUS_TYPE_SESSION,nullptr,nullptr);if(!bus)fail("private bus unavailable");change_scale();started=changed=g_get_monotonic_time();g_timeout_add(30,tick,nullptr);return G_SOURCE_REMOVE;
}
__attribute__((constructor))static void setup(){char exe[2048];auto n=readlink("/proc/self/exe",exe,sizeof(exe)-1);if(n<0)return;exe[n]=0;if(strstr(exe,"/.gnosis-wrapped"))g_timeout_add_seconds(3,start,nullptr);}
