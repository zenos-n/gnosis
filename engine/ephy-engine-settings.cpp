/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "ephy-engine-settings.h"
#include "ephy-engine-session.h"
#include "brave/browser.hpp"
#include <cstring>
#include <mutex>

namespace {
std::mutex request_mutex;
std::string request_agent, request_languages;
bool valid_header(const char *text) {
  for (auto *p = reinterpret_cast<const unsigned char*>(text); p && *p; ++p)
    if ((*p < 32 && *p != '\t') || *p == 127) return false;
  return true;
}
}

void ephy_engine_views_preferences_changed(const char *property);

enum {
  NONE, STANDARD, SERIF, SANS, MONOSPACE, FONT_SIZE, FIXED_SIZE, MIN_SIZE,
  ENCODING, LANGUAGES, SPELL_CHECK, CARET, STORAGE, HARDWARE, USER_AGENT,
  FEATURES, POPUPS, ADBLOCK, COOKIE_BANNERS, VIM_MODE, NAVIGATION_GESTURES, ADAPTERS, N_PROPERTIES
};
static GParamSpec *properties[N_PROPERTIES];
struct _EphyEngineSettings { GObject parent; GValue values[N_PROPERTIES]; };
G_DEFINE_TYPE(EphyEngineSettings, ephy_engine_settings, G_TYPE_OBJECT)

static void ephy_engine_settings_class_init(EphyEngineSettingsClass *klass) {
  auto *object = G_OBJECT_CLASS(klass);
  object->get_property = +[](GObject *object, guint id, GValue *value, GParamSpec*) {
    g_value_copy(&EPHY_ENGINE_SETTINGS(object)->values[id], value);
  };
  object->set_property = +[](GObject *object, guint id, const GValue *value, GParamSpec*) {
    if (id == USER_AGENT && !valid_header(g_value_get_string(value))) {
      g_warning("The user agent contains invalid HTTP header characters");
      return;
    }
    if (id == LANGUAGES) {
      auto **languages = static_cast<char**>(g_value_get_boxed(value));
      for (unsigned i = 0; languages && languages[i]; ++i) {
        if (!*languages[i] || strlen(languages[i]) > 63 || strspn(languages[i], "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789-") != strlen(languages[i])) {
          g_warning("Preferred languages must contain language tags such as en-GB");
          return;
        }
      }
    }
    g_value_copy(value, &EPHY_ENGINE_SETTINGS(object)->values[id]);
    if (id == USER_AGENT || id == LANGUAGES) {
      std::lock_guard lock(request_mutex);
      if (id == USER_AGENT) request_agent = g_value_get_string(value) ? g_value_get_string(value) : "";
      else {
        request_languages.clear();
        auto **languages = static_cast<char**>(g_value_get_boxed(value));
        for (unsigned i = 0; languages && languages[i]; ++i) {
          if (i) request_languages += ",";
          request_languages += languages[i];
          if (i) request_languages += ";q=0." + std::to_string(i < 10 ? 10 - i : 1);
        }
      }
    }
  };
  object->finalize = +[](GObject *object) {
    for (guint i = 1; i < N_PROPERTIES; ++i) g_value_unset(&EPHY_ENGINE_SETTINGS(object)->values[i]);
    G_OBJECT_CLASS(ephy_engine_settings_parent_class)->finalize(object);
  };
  auto flags = GParamFlags(G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS);
  properties[STANDARD] = g_param_spec_string("default-font-family", nullptr, nullptr, "serif", flags);
  properties[SERIF] = g_param_spec_string("serif-font-family", nullptr, nullptr, "serif", flags);
  properties[SANS] = g_param_spec_string("sans-serif-font-family", nullptr, nullptr, "sans-serif", flags);
  properties[MONOSPACE] = g_param_spec_string("monospace-font-family", nullptr, nullptr, "monospace", flags);
  properties[FONT_SIZE] = g_param_spec_uint("default-font-size", nullptr, nullptr, 0, G_MAXINT, 16, flags);
  properties[FIXED_SIZE] = g_param_spec_uint("default-monospace-font-size", nullptr, nullptr, 0, G_MAXINT, 13, flags);
  properties[MIN_SIZE] = g_param_spec_uint("minimum-font-size", nullptr, nullptr, 0, G_MAXINT, 0, flags);
  properties[ENCODING] = g_param_spec_string("default-charset", nullptr, nullptr, "UTF-8", flags);
  properties[LANGUAGES] = g_param_spec_boxed("preferred-languages", nullptr, nullptr, G_TYPE_STRV, flags);
  properties[SPELL_CHECK] = g_param_spec_boolean("enable-spell-checking", nullptr, nullptr, true, flags);
  properties[CARET] = g_param_spec_boolean("enable-caret-browsing", nullptr, nullptr, false, flags);
  properties[STORAGE] = g_param_spec_boolean("enable-website-data-storage", nullptr, nullptr, true, flags);
  properties[HARDWARE] = g_param_spec_boolean("hardware-acceleration", nullptr, nullptr, true, flags);
  properties[USER_AGENT] = g_param_spec_string("user-agent", nullptr, nullptr, "", flags);
  properties[POPUPS] = g_param_spec_boolean("allow-popups", nullptr, nullptr, false, flags);
  properties[ADBLOCK] = g_param_spec_boolean("enable-adblock", nullptr, nullptr, true, flags);
  properties[COOKIE_BANNERS] = g_param_spec_boolean("block-cookie-banners", nullptr, nullptr, false, flags);
  properties[VIM_MODE] = g_param_spec_boolean("enable-vim-mode", nullptr, nullptr, false, flags);
  properties[NAVIGATION_GESTURES] = g_param_spec_boolean("enable-navigation-gestures", nullptr, nullptr, true, flags);
  properties[ADAPTERS] = g_param_spec_variant("brave-adapter-overrides", nullptr, nullptr, G_VARIANT_TYPE("a{sb}"), nullptr, flags);
  properties[FEATURES] = g_param_spec_variant("feature-overrides", nullptr, nullptr, G_VARIANT_TYPE("a{sb}"), nullptr, flags);
  g_object_class_install_properties(object, N_PROPERTIES, properties);
}
void ephy_engine_settings_apply_request(CefRefPtr<CefRequest> request, const std::string *override_agent) {
  // CEF can retain the initial language header after profile preferences change.
  std::lock_guard lock(request_mutex);
  const auto& agent = override_agent ? *override_agent : request_agent;
  if (!agent.empty() && valid_header(agent.c_str())) request->SetHeaderByName("User-Agent", agent, true);
  if (!request_languages.empty()) request->SetHeaderByName("Accept-Language", request_languages, true);
}
namespace {
class SettingsRequestHandler final : public CefResourceRequestHandler {
public:
  SettingsRequestHandler(CefRefPtr<CefCookieAccessFilter> filter, CefRefPtr<CefResourceRequestHandler> next)
    : filter_(filter), next_(next) {}
  CefRefPtr<CefCookieAccessFilter> GetCookieAccessFilter(CefRefPtr<CefBrowser>, CefRefPtr<CefFrame>, CefRefPtr<CefRequest>) override { return filter_; }
private:
  cef_return_value_t OnBeforeResourceLoad(CefRefPtr<CefBrowser> browser, CefRefPtr<CefFrame> frame, CefRefPtr<CefRequest> request,
                                         CefRefPtr<CefCallback> callback) override {
    ephy_engine_settings_apply_request(request);
    return next_ ? next_->OnBeforeResourceLoad(browser, frame, request, callback) : RV_CONTINUE;
  }
  CefRefPtr<CefCookieAccessFilter> filter_;
  CefRefPtr<CefResourceRequestHandler> next_;
  IMPLEMENT_REFCOUNTING(SettingsRequestHandler);
};
}
CefRefPtr<CefResourceRequestHandler> ephy_engine_settings_request_handler(CefRefPtr<CefCookieAccessFilter> filter,
  CefRefPtr<CefResourceRequestHandler> next) { return new SettingsRequestHandler(filter, next); }
static void publish_native_preferences(EphyEngineSettings* settings) {
  g_autoptr(JsonObject) root = json_object_new();
  auto* fonts = json_object_new(); json_object_set_object_member(root, "fonts", fonts);
  for (auto [id, name] : {std::pair{STANDARD,"standard"}, {SERIF,"serif"}, {SANS,"sansserif"}, {MONOSPACE,"fixed"}})
    json_object_set_string_member(fonts, name, g_value_get_string(&settings->values[id]));
  for (auto [id, name] : {std::pair{FONT_SIZE,"fontSize"}, {FIXED_SIZE,"fixedSize"}, {MIN_SIZE,"minimumSize"}})
    json_object_set_int_member(root, name, g_value_get_uint(&settings->values[id]));
  for (auto [id, name] : {std::pair{STORAGE,"storage"}, {POPUPS,"popups"}, {ADBLOCK,"adblock"}, {COOKIE_BANNERS,"cookieBanners"}, {VIM_MODE,"vimMode"}, {NAVIGATION_GESTURES,"navigationGestures"}})
    json_object_set_boolean_member(root, name, g_value_get_boolean(&settings->values[id]));
  auto* adapters = json_object_new(); json_object_set_object_member(root, "adapters", adapters);
  if (auto* overrides = g_value_get_variant(&settings->values[ADAPTERS])) {
    GVariantIter iter; const char* id; gboolean enabled;
    g_variant_iter_init(&iter, overrides);
    while (g_variant_iter_loop(&iter, "{&sb}", &id, &enabled)) json_object_set_boolean_member(adapters, id, enabled);
  }
  g_autoptr(JsonNode) node = json_node_new(JSON_NODE_OBJECT); json_node_set_object(node, root);
  g_autofree char* json = json_to_string(node, false);
  GnosisBrave::update_preferences(json);
}
gboolean ephy_engine_debug_adapter_enabled(const char *id) { return GnosisBrave::adapter_enabled(id); }
static void ephy_engine_settings_init(EphyEngineSettings *settings) {
  for (guint i = 1; i < N_PROPERTIES; ++i) {
    g_value_init(&settings->values[i], G_PARAM_SPEC_VALUE_TYPE(properties[i]));
    g_param_value_set_default(properties[i], &settings->values[i]);
  }
  g_signal_connect(settings, "notify", G_CALLBACK(+[](GObject* object, GParamSpec *spec, gpointer) {
    publish_native_preferences(EPHY_ENGINE_SETTINGS(object));
    ephy_engine_sessions_preferences_changed();
    ephy_engine_views_preferences_changed(spec->name);
  }), nullptr);
}
EphyEngineSettings *ephy_engine_settings_get_default() {
  static auto *settings = EPHY_ENGINE_SETTINGS(g_object_new(EPHY_TYPE_ENGINE_SETTINGS, nullptr));
  return settings;
}
gboolean ephy_engine_settings_get_storage_enabled() {
  return g_value_get_boolean(&ephy_engine_settings_get_default()->values[STORAGE]);
}
gboolean ephy_engine_settings_get_hardware_acceleration() {
  return g_value_get_boolean(&ephy_engine_settings_get_default()->values[HARDWARE]);
}
char *ephy_engine_settings_dup_user_agent() {
  return g_value_dup_string(&ephy_engine_settings_get_default()->values[USER_AGENT]);
}
void ephy_engine_settings_apply_context(CefRefPtr<CefRequestContext> context) {
  auto *settings = ephy_engine_settings_get_default();
  auto set = [&](const char *name, CefRefPtr<CefValue> value) {
    CefString error;
    if (!context->SetPreference(name, value, error))
      g_warning("Could not apply Chromium preference %s: %s", name, error.ToString().c_str());
  };
  for (const auto& [id, name] : {
    std::pair{STANDARD, "webkit.webprefs.fonts.standard.Zyyy"}, {SERIF, "webkit.webprefs.fonts.serif.Zyyy"},
    {SANS, "webkit.webprefs.fonts.sansserif.Zyyy"}, {MONOSPACE, "webkit.webprefs.fonts.fixed.Zyyy"},
    {FONT_SIZE, "webkit.webprefs.default_font_size"}, {FIXED_SIZE, "webkit.webprefs.default_fixed_font_size"},
    {MIN_SIZE, "webkit.webprefs.minimum_font_size"}, {ENCODING, "intl.charset_default"},
    {SPELL_CHECK, "browser.enable_spellchecking"}, {CARET, "settings.a11y.caretbrowsing.enabled"}}) {
    const auto *input = &settings->values[id];
    auto value = CefValue::Create();
    if (G_VALUE_HOLDS_STRING(input)) value->SetString(g_value_get_string(input));
    else if (G_VALUE_HOLDS_UINT(input)) value->SetInt(g_value_get_uint(input));
    else value->SetBool(g_value_get_boolean(input));
    set(name, value);
  }
  auto **languages = static_cast<char**>(g_value_get_boxed(&settings->values[LANGUAGES]));
  if (languages && *languages) {
    char *joined = g_strjoinv(",", languages);
    auto value = CefValue::Create(); value->SetString(joined); g_free(joined);
    set("intl.selected_languages", value);
    set("intl.accept_languages", value);
    auto dictionaries = CefListValue::Create();
    for (unsigned i = 0; languages[i]; ++i) dictionaries->SetString(i, languages[i]);
    value = CefValue::Create(); value->SetList(dictionaries);
    set("spellcheck.dictionaries", value);
  }
}

static const EphyEngineFeature features[] = {
  {"webgl", "WebGL", "Graphics", "Hardware accelerated 3D graphics. Changes take effect after restarting Gnosis.", true, nullptr, "disable-webgl", nullptr},
  {"webgpu", "WebGPU", "Graphics", "GPU rendering and compute. Changes take effect after restarting Gnosis.", true, nullptr, nullptr, "WebGPUService"},
  {"back-forward-cache", "Back/forward cache", "Navigation", "Keep pages in memory for faster history navigation. Changes take effect after restarting Gnosis.", true, nullptr, nullptr, "BackForwardCache"},
  {"smooth-scrolling", "Smooth scrolling", "Navigation", "Animate page scrolling. Changes take effect after restarting Gnosis.", true, "enable-smooth-scrolling", "disable-smooth-scrolling", nullptr},
  {"experimental-web-platform", "Experimental web platform features", "Web Platform", "Enable experimental Chromium web APIs. Changes take effect after restarting Gnosis.", false, "enable-experimental-web-platform-features", nullptr, nullptr},
};
const EphyEngineFeature *ephy_engine_settings_get_features(gsize *length) { *length = G_N_ELEMENTS(features); return features; }
gboolean ephy_engine_settings_get_feature_enabled(EphyEngineSettings *settings, const EphyEngineFeature *feature) {
  auto *overrides = g_value_get_variant(&settings->values[FEATURES]);
  gboolean enabled;
  return overrides && g_variant_lookup(overrides, feature->id, "b", &enabled) ? enabled : feature->default_value;
}
void ephy_engine_settings_set_feature_enabled(EphyEngineSettings *settings, const EphyEngineFeature *feature, gboolean enabled) {
  auto *current = g_value_get_variant(&settings->values[FEATURES]);
  GVariantBuilder overrides;
  g_variant_builder_init(&overrides, G_VARIANT_TYPE("a{sb}"));
  if (current) {
    GVariantIter iter;
    const char *key;
    gboolean previous;
    g_variant_iter_init(&iter, current);
    while (g_variant_iter_next(&iter, "{&sb}", &key, &previous))
      if (strcmp(key, feature->id)) g_variant_builder_add(&overrides, "{sb}", key, previous);
  }
  if (enabled != feature->default_value) g_variant_builder_add(&overrides, "{sb}", feature->id, enabled);
  auto *value = g_variant_ref_sink(g_variant_builder_end(&overrides));
  g_object_set(settings, "feature-overrides", value, nullptr);
  g_variant_unref(value);
}
void ephy_engine_settings_configure_command_line(CefRefPtr<CefCommandLine> command) {
  auto *settings = ephy_engine_settings_get_default();
  auto disabled = command->GetSwitchValue("disable-features").ToString();
  command->AppendSwitchWithValue("disable-features", disabled + (disabled.empty() ? "" : ",") + "MediaRouter");
  if (!ephy_engine_settings_get_hardware_acceleration()) {
    command->AppendSwitch("disable-gpu");
    command->AppendSwitch("disable-gpu-compositing");
  }
  for (const auto& feature : features) {
    bool enabled = ephy_engine_settings_get_feature_enabled(settings, &feature);
    if (auto *name = enabled ? feature.enable_switch : feature.disable_switch) command->AppendSwitch(name);
    if (feature.feature_flag) {
      const char *option = enabled ? "enable-features" : "disable-features";
      auto list = command->GetSwitchValue(option).ToString();
      command->AppendSwitchWithValue(option, list + (list.empty() ? "" : ",") + feature.feature_flag);
    }
  }
}
