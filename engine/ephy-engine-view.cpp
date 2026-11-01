/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "ephy-engine-view.h"
#include "ephy-engine-scheme.h"
#include "ephy-engine-extensions.h"
#include "ephy-engine-certificate.hpp"
#include "select/application.hpp"
#include "select/clipboard.hpp"
#include "select/page-messages.hpp"
#include "select/policy.hpp"
#include "select/devtools.hpp"
#include "select/process-dumps.hpp"
#include "select/extension-runtime.hpp"
#include "include/cef_app.h"
#include "include/cef_parser.h"
#include "include/cef_task.h"
#include "include/cef_version_info.h"
#include "include/cef_pack_strings.h"
#include "include/cef_resource_bundle_handler.h"
#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <filesystem>
#include <map>
#include <memory>
#include <set>
#include <vector>

namespace {
using Reply = std::function<void(CefRefPtr<CefDictionaryValue>, const std::string&)>;
class ScriptEvaluation;
class Protocol final : public CefDevToolsMessageObserver {
public:
  CefRefPtr<CefBrowser> browser;
  std::weak_ptr<GnosisBrave::Page> brave;
  CefRefPtr<CefRegistration> registration;
  std::map<int, Reply> pending;
  std::map<ScriptEvaluation*, std::weak_ptr<ScriptEvaluation>> evaluations;
  void notify_evaluations();
  std::function<void()> on_same_document;
  struct Context { std::string frame, unique; bool main = false; };
  std::map<int, Context> contexts;
  std::string main_frame;
  guint64 document_epoch = 0;
  int send(const char* method, CefRefPtr<CefDictionaryValue> args, Reply done = {}) {
    if (auto page = brave.lock()) {
      const int id = next_devtools_message_id();
      if (done) pending[id] = std::move(done);
      auto value = CefValue::Create();
      if (args) value->SetDictionary(args->Copy(false));
      const auto params = args ? CefWriteJSON(value, JSON_WRITER_DEFAULT).ToString() : "{}";
      CefRefPtr<Protocol> current = this;
      page->command(method, params, [current, id](JsonObject* result, const std::string& error) {
        auto found = current->pending.find(id); if (found == current->pending.end()) return;
        auto callback = std::move(found->second); current->pending.erase(found);
        CefRefPtr<CefDictionaryValue> converted;
        if (result) {
          auto* node = json_node_new(JSON_NODE_OBJECT); json_node_set_object(node, result);
          char* json = json_to_string(node, false);
          auto value = CefParseJSON(json, JSON_PARSER_RFC); if (value && value->GetType() == VTYPE_DICTIONARY) converted = value->GetDictionary();
          g_free(json); json_node_free(node);
        }
        callback(converted, error);
      });
      return id;
    }
    if (!browser) { if (done) done(nullptr, "Browser is not ready"); return 0; }
    int id = next_devtools_message_id();
    if (done) pending[id] = std::move(done);
    if (!send_devtools_method(browser, id, method, args)) fail(id, "Chromium rejected the operation");
    return id;
  }
  void OnDevToolsEvent(CefRefPtr<CefBrowser>, const CefString& method, const void* json, size_t size) override {
    if (method != "Runtime.executionContextCreated" && method != "Runtime.executionContextDestroyed" &&
        method != "Runtime.executionContextsCleared" && method != "Page.frameNavigated" &&
        method != "Page.navigatedWithinDocument") return;
    auto value = CefParseJSON(static_cast<const char*>(json), size, JSON_PARSER_RFC);
    auto args = value && value->GetType() == VTYPE_DICTIONARY ? value->GetDictionary() : nullptr;
    if (!args) return;
    if (method == "Page.navigatedWithinDocument") {
      if (args->GetString("frameId").ToString() == main_frame && on_same_document) {
        // Sending another DevTools command inside its event dispatch is reentrant.
        g_idle_add_full(G_PRIORITY_DEFAULT, +[](gpointer p) -> gboolean {
          auto current = *static_cast<CefRefPtr<Protocol>*>(p);
          if (current->browser && current->on_same_document) current->on_same_document();
          return G_SOURCE_REMOVE;
        }, new CefRefPtr<Protocol>(this), +[](gpointer p) { delete static_cast<CefRefPtr<Protocol>*>(p); });
      }
      return;
    }
    if (method == "Runtime.executionContextCreated") {
      auto context = args->GetDictionary("context");
      auto aux = context ? context->GetDictionary("auxData") : nullptr;
      if (context && aux)
        contexts[context->GetInt("id")] = {aux->GetString("frameId"), context->GetString("uniqueId"), aux->GetBool("isDefault")};
    } else if (method == "Runtime.executionContextDestroyed") {
      auto it = contexts.find(args->GetInt("executionContextId"));
      if (it != contexts.end() && (!args->HasKey("executionContextUniqueId") ||
          it->second.unique == args->GetString("executionContextUniqueId").ToString())) {
        if (it->second.main && it->second.frame == main_frame) ++document_epoch;
        contexts.erase(it);
      }
    } else if (method == "Runtime.executionContextsCleared") {
      contexts.clear();
      ++document_epoch;
    } else if (method == "Page.frameNavigated") {
      auto frame = args->GetDictionary("frame");
      if (frame && !frame->HasKey("parentId")) {
        main_frame = frame->GetString("id");
        ++document_epoch;
      }
    }
    if (method != "Runtime.executionContextCreated") notify_evaluations();
  }
  void OnDevToolsMethodResult(CefRefPtr<CefBrowser>, int id, bool ok, const void* json, size_t size) override {
    auto it = pending.find(id);
    if (it == pending.end()) return;
    auto value = CefParseJSON(static_cast<const char*>(json), size, JSON_PARSER_RFC);
    auto result = value && value->GetType() == VTYPE_DICTIONARY ? value->GetDictionary() : nullptr;
    struct Delivery { CefRefPtr<Protocol> protocol; int id; CefRefPtr<CefDictionaryValue> result; bool ok; };
    g_idle_add_full(G_PRIORITY_DEFAULT, +[](gpointer p) -> gboolean {
      auto* d = static_cast<Delivery*>(p);
      auto it = d->protocol->pending.find(d->id);
      if (it != d->protocol->pending.end()) {
        auto done = std::move(it->second); d->protocol->pending.erase(it);
        auto error = d->result ? d->result->GetString("message").ToString() : "";
        done(d->result, d->ok ? "" : error.empty() ? "Chromium rejected the operation" : error);
      }
      return G_SOURCE_REMOVE;
    }, new Delivery{this, id, result, ok}, +[](gpointer p) { delete static_cast<Delivery*>(p); });
  }
  void fail(int id, const char *message) {
    auto it = pending.find(id);
    if (it == pending.end()) return;
    auto done = std::move(it->second); pending.erase(it);
    done(nullptr, message);
  }
  void cancel_pending(const char *message) {
    auto calls = std::move(pending);
    for (auto& [id, reply] : calls) reply(nullptr, message);
  }
  void stop() {
    registration = nullptr; browser = nullptr; brave.reset();
    on_same_document = nullptr;
    contexts.clear();
    cancel_pending("Tab was closed");
  }
private:
  IMPLEMENT_REFCOUNTING(Protocol);
};
struct ViewData;
Application* runtime = nullptr;
std::vector<ViewData*> views;
CefRefPtr<CefApp> cef_app;
guint pump = 0;
}

struct _EphyEngineHistoryItem { GObject parent; char* uri; char* title; int id; bool alternate; };
typedef struct _EphyEngineHistoryItemClass { GObjectClass parent; } EphyEngineHistoryItemClass;
G_DEFINE_TYPE(EphyEngineHistoryItem, ephy_engine_history_item, G_TYPE_OBJECT)
static void ephy_engine_history_item_class_init(EphyEngineHistoryItemClass* klass) {
  G_OBJECT_CLASS(klass)->finalize = +[](GObject* object) {
    auto* item = reinterpret_cast<EphyEngineHistoryItem*>(object);
    g_free(item->uri); g_free(item->title);
    G_OBJECT_CLASS(ephy_engine_history_item_parent_class)->finalize(object);
  };
}
static void ephy_engine_history_item_init(EphyEngineHistoryItem*) {}
struct _EphyEngineHistory { GObject parent; ViewData* view; std::vector<EphyEngineHistoryItem*>* items; int current; };
typedef struct _EphyEngineHistoryClass { GObjectClass parent; } EphyEngineHistoryClass;
G_DEFINE_TYPE(EphyEngineHistory, ephy_engine_history, G_TYPE_OBJECT)
static void ephy_engine_history_class_init(EphyEngineHistoryClass* klass) {
  G_OBJECT_CLASS(klass)->finalize = +[](GObject* object) {
    auto* h = reinterpret_cast<EphyEngineHistory*>(object);
    for (auto* item : *h->items) g_object_unref(item);
    delete h->items;
    G_OBJECT_CLASS(ephy_engine_history_parent_class)->finalize(object);
  };
  g_signal_new("changed", G_TYPE_FROM_CLASS(klass), G_SIGNAL_RUN_LAST, 0, nullptr, nullptr, nullptr,
    G_TYPE_NONE, 2, G_TYPE_POINTER, G_TYPE_POINTER);
}
static void ephy_engine_history_init(EphyEngineHistory* h) { h->items = new std::vector<EphyEngineHistoryItem*>; h->current = -1; }
struct _EphyEngineFind { GObject parent; ViewData* view; char* text; guint options; guint64 generation; };
typedef struct _EphyEngineFindClass { GObjectClass parent; } EphyEngineFindClass;
G_DEFINE_TYPE(EphyEngineFind, ephy_engine_find, G_TYPE_OBJECT)
static void ephy_engine_find_class_init(EphyEngineFindClass* klass) {
  G_OBJECT_CLASS(klass)->finalize = +[](GObject* object) {
    g_free(reinterpret_cast<EphyEngineFind*>(object)->text);
    G_OBJECT_CLASS(ephy_engine_find_parent_class)->finalize(object);
  };
  for (auto* name : {"found-text", "counted-matches"})
    g_signal_new(name, G_TYPE_FROM_CLASS(klass), G_SIGNAL_RUN_LAST, 0, nullptr, nullptr, nullptr, G_TYPE_NONE, 1, G_TYPE_UINT);
  g_signal_new("failed-to-find-text", G_TYPE_FROM_CLASS(klass), G_SIGNAL_RUN_LAST, 0, nullptr, nullptr, nullptr, G_TYPE_NONE, 0);
  g_signal_new("search-result", G_TYPE_FROM_CLASS(klass), G_SIGNAL_RUN_LAST, 0, nullptr, nullptr, nullptr,
    G_TYPE_NONE, 2, G_TYPE_UINT, G_TYPE_UINT);
}
static void ephy_engine_find_init(EphyEngineFind*) {}
struct _EphyEngineInspector { GObject parent; ViewData* view; };
typedef struct _EphyEngineInspectorClass { GObjectClass parent; } EphyEngineInspectorClass;
G_DEFINE_TYPE(EphyEngineInspector, ephy_engine_inspector, G_TYPE_OBJECT)
static void ephy_engine_inspector_class_init(EphyEngineInspectorClass* klass) {
  for (auto* name : {"attach", "closed"})
    g_signal_new(name, G_TYPE_FROM_CLASS(klass), G_SIGNAL_RUN_LAST, 0, g_signal_accumulator_true_handled, nullptr, nullptr, G_TYPE_BOOLEAN, 0);
}
static void ephy_engine_inspector_init(EphyEngineInspector*) {}
struct _EphyEngineResource { GObject parent; GWeakRef view; char *uri; EphyEngineResponse *response; };
typedef struct _EphyEngineResourceClass { GObjectClass parent; } EphyEngineResourceClass;
G_DEFINE_TYPE(EphyEngineResource, ephy_engine_resource, G_TYPE_OBJECT)
static void ephy_engine_resource_class_init(EphyEngineResourceClass *klass) {
  G_OBJECT_CLASS(klass)->finalize = +[](GObject *object) {
    auto *resource = reinterpret_cast<EphyEngineResource*>(object);
    g_weak_ref_clear(&resource->view); g_free(resource->uri); g_clear_object(&resource->response);
    G_OBJECT_CLASS(ephy_engine_resource_parent_class)->finalize(object);
  };
  g_signal_new("finished", G_TYPE_FROM_CLASS(klass), G_SIGNAL_RUN_LAST, 0,
    nullptr, nullptr, nullptr, G_TYPE_NONE, 0);
}
static void ephy_engine_resource_init(EphyEngineResource *resource) { g_weak_ref_init(&resource->view, nullptr); }
static EphyEngineResource *new_resource(EphyEngineView *view, const char *uri) {
  auto *resource = reinterpret_cast<EphyEngineResource*>(g_object_new(ephy_engine_resource_get_type(), nullptr));
  g_weak_ref_set(&resource->view, view); resource->uri = g_strdup(uri);
  return resource;
}
struct _EphyEngineSessionState { gatomicrefcount refs; GBytes* bytes; };
namespace {
struct ViewData {
  EphyEngineView* self;
  GWeakRef extension_owner;
  Window host;
  CefRefPtr<Browser> browser;
  CefRefPtr<Protocol> protocol;
  EphyEngineHistory* history;
  EphyEngineFind* find;
  EphyEngineInspector* inspector;
  EphyEngineResource* resource;
  std::map<int, EphyEngineResponse*> history_responses;
  std::map<std::string, std::string> alternate_uris;
  std::set<std::string> page_message_handlers;
  EphyEngineSession* session = nullptr;
  WebKitUserContentManager* ucm = nullptr;
  EphyEngineSettings* settings = nullptr;
  EphyEnginePopupProperties popup_properties{};
  bool creating_popup = false;
  bool native_popup = false;
  GdkTexture* favicon = nullptr;
  std::string uri, title, charset;
  std::string user_agent_override;
  bool has_user_agent_override = false;
  bool loading = false, muted = false, disposed = false, detached = false;
  GCancellable* external_uri = nullptr;
  guint extension_context_timer = 0;
  std::string extension_context_visit;
  bool extension_context_reload_used = false;
  guint64 extension_context_generation = 0;
  double progress = 0, zoom = 1;
  explicit ViewData(EphyEngineView* view) : self(view), host(*runtime) {
    g_weak_ref_init(&extension_owner, nullptr);
    browser = new Browser("about:blank", g_getenv("GNOSIS_SOFTWARE_RENDERING") || !ephy_engine_settings_get_hardware_acceleration());
    browser->host = &host;
    settings = EPHY_ENGINE_SETTINGS(g_object_ref(ephy_engine_settings_get_default()));
    protocol = new Protocol;
    protocol->on_same_document = [this] { if (!disposed) update_history(false); };
    history = reinterpret_cast<EphyEngineHistory*>(g_object_new(ephy_engine_history_get_type(), nullptr)); history->view = this;
    find = reinterpret_cast<EphyEngineFind*>(g_object_new(ephy_engine_find_get_type(), nullptr)); find->view = this;
    inspector = reinterpret_cast<EphyEngineInspector*>(g_object_new(ephy_engine_inspector_get_type(), nullptr)); inspector->view = this;
    resource = new_resource(view, "about:blank");
    ucm = webkit_user_content_manager_new();
    browser->on_changed = [this] { changed(); };
    browser->on_popup_size = [this](int width, int height) {
      if (disposed || detached || !browser->is_extension_popup) return;
      auto* root = gtk_widget_get_root(GTK_WIDGET(self));
      if (!root) return;
      const int max_width = std::max(100, std::min(800, gtk_widget_get_width(GTK_WIDGET(root)) - 32));
      const int max_height = std::max(100, std::min(600, gtk_widget_get_height(GTK_WIDGET(root)) - 100));
      gtk_widget_set_size_request(GTK_WIDGET(self), std::clamp(width, 25, max_width),
                                  std::clamp(height, 25, max_height));
      g_signal_emit_by_name(self, "extension-popup-size-changed");
    };
    browser->on_security_changed = [this] { if (!disposed) g_signal_emit_by_name(self, "tls-info-changed"); };
    browser->on_fullscreen_changed = [this](bool value) {
      if (disposed) return;
      gboolean handled = false;
      g_signal_emit_by_name(self, value ? "enter-fullscreen" : "leave-fullscreen", &handled);
    };
    browser->on_create_popup = [this](const std::string&, const CefPopupFeatures& features) -> CefRefPtr<Browser> {
      if (disposed || detached) return nullptr;
      g_autoptr(EphyEngineView) keep_alive = EPHY_ENGINE_VIEW(g_object_ref(self));
      g_autoptr(EphyEngineView) owner = browser->is_extension_popup ?
        EPHY_ENGINE_VIEW(g_weak_ref_get(&extension_owner)) : EPHY_ENGINE_VIEW(g_object_ref(self));
      if (!owner || data(owner)->disposed || data(owner)->detached ||
          !gtk_widget_get_root(GTK_WIDGET(owner)) ||
          (browser->is_extension_popup && data(owner)->host.private_mode)) return nullptr;
      auto *parent = data(owner);
      EphyEnginePopupProperties properties{
        {features.x, features.y, features.width, features.height},
        features.xSet, features.ySet, features.widthSet, features.heightSet, features.isPopup
      };
      GtkWidget *child = nullptr;
      parent->creating_popup = true;
      g_signal_emit_by_name(owner, "create-popup", &properties, &child);
      parent->creating_popup = false;
      if (!child || !EPHY_IS_ENGINE_VIEW(child)) return nullptr;
      auto *popup = data(EPHY_ENGINE_VIEW(child));
      if (disposed || popup->disposed || popup == this || popup->session != session ||
          popup->host.private_mode != host.private_mode || !popup->browser->defer_start ||
          popup->browser->loaded || popup->browser->closing) return nullptr;
      popup->popup_properties = properties;
      popup->native_popup = true;
      return popup->browser;
    };
    browser->on_ready = [this] {
      if (!native_popup || disposed || detached) return;
      g_autoptr(EphyEngineView) keep_alive = EPHY_ENGINE_VIEW(g_object_ref(self));
      g_signal_emit_by_name(self, "ready-to-show");
    };
    browser->on_contents_bounds_change = [this](const CefRect& bounds) {
      if (!native_popup || disposed || detached || !popup_properties.is_popup) return;
      popup_properties.geometry = {bounds.x, bounds.y, bounds.width, bounds.height};
      popup_properties.x_set = popup_properties.y_set = true;
      popup_properties.width_set = popup_properties.height_set = true;
      g_autoptr(EphyEngineView) keep_alive = EPHY_ENGINE_VIEW(g_object_ref(self));
      g_signal_emit_by_name(self, "popup-geometry-changed");
    };

    browser->on_page_message = [this](const std::string& url, const std::string& name, const std::string& payload) {
      if (disposed || detached || !page_message_handlers.contains(name) ||
          (!alternate_uris.contains(url) && !url.starts_with("ephy-about:"))) return;
      g_autoptr(JsonParser) parser = json_parser_new();
      if (!json_parser_load_from_data(parser, payload.c_str(), payload.size(), nullptr)) return;
      g_autoptr(EphyEngineView) keep_alive = EPHY_ENGINE_VIEW(g_object_ref(self));
      g_signal_emit(self, g_signal_lookup("page-message", EPHY_TYPE_ENGINE_VIEW),
                    g_quark_from_string(name.c_str()), json_parser_get_root(parser));
    };
    browser->on_authentication = [this](EphyEngineAuthentication *request) {
      if (disposed || detached || !gtk_widget_get_mapped(GTK_WIDGET(self))) return false;
      g_autoptr(EphyEngineView) keep_alive = EPHY_ENGINE_VIEW(g_object_ref(self));
      gboolean handled = false;
      g_signal_emit_by_name(self, "authentication-request", request, &handled);
      return bool(handled);
    };
    browser->on_client_certificate = [this](EphyEngineClientCertificate *request) {
      if (disposed || detached || !gtk_widget_get_mapped(GTK_WIDGET(self))) return false;
      g_autoptr(EphyEngineView) keep_alive = EPHY_ENGINE_VIEW(g_object_ref(self));
      gboolean handled = false;
      g_signal_emit_by_name(self, "client-certificate-request", request, &handled);
      return bool(handled);
    };
    browser->on_permission = [this](EphyEnginePermission *request) {
      if (disposed || detached) return false;
      gboolean handled = false;
      g_signal_emit_by_name(self, "permission-request", request, &handled);
      return bool(handled);
    };
    browser->on_webstore_install_blocked = [this] {
      if (disposed || detached || !gtk_widget_get_mapped(GTK_WIDGET(self))) return;
      g_autoptr(EphyEngineView) keep_alive = EPHY_ENGINE_VIEW(g_object_ref(self));
      g_signal_emit_by_name(self, "webstore-install-blocked");
    };
    browser->on_external_uri = [this](const std::string& uri, const std::string& context, bool initiator_known) {
      if (disposed || detached || !gtk_widget_get_mapped(GTK_WIDGET(self))) return;
      g_autoptr(EphyEngineView) keep_alive = EPHY_ENGINE_VIEW(g_object_ref(self));
      cancel_external_uri();
      external_uri = g_cancellable_new();
      gboolean handled = false;
      g_signal_emit_by_name(self, "open-external-uri", uri.c_str(), context.c_str(), initiator_known,
                            external_uri, &handled);
      if (!handled) cancel_external_uri();
    };
    browser->on_favicon = [this](const std::string& page, const std::string& icon, GdkTexture* texture) {
      if (disposed) return;
      if (texture && !host.private_mode)
        ephy_engine_favicon_database_store(ephy_engine_favicon_database_get_default(), page.c_str(), icon.c_str(), texture);
      if (page == browser->url && g_set_object(&favicon, texture))
        g_object_notify(G_OBJECT(self), "favicon");
    };
    browser->on_closed = [this] { gboolean handled = FALSE; g_signal_emit_by_name(self, "close", &handled); };
    browser->prepare_navigation = [this] { apply_user_agent(); };
    browser->on_load_start = [this](const std::string& url) {
      cancel_external_uri();
      stop_extension_context_probe();
      ++extension_context_generation;
      const auto visit = url.substr(0, url.find('#'));
      if (extension_context_visit != visit) {
        extension_context_visit = visit;
        extension_context_reload_used = false;
      }
      g_clear_object(&resource);
      resource = new_resource(self, visible_uri(url).c_str());
      g_signal_emit_by_name(self, "load-changed", WEBKIT_LOAD_STARTED);
    };
    browser->on_response = [this](const auto& uri, auto response) {
      g_free(resource->uri); resource->uri = g_strdup(visible_uri(uri).c_str());
      g_clear_object(&resource->response);
      resource->response = ephy_engine_response_new(uri, response);
    };
    browser->on_resource_complete = [this](const std::string& uri, CefRefPtr<CefResponse> response) {
      if (disposed || detached) return;
      auto signal = g_signal_lookup("resource-response", EPHY_TYPE_ENGINE_VIEW);
      if (!g_signal_has_handler_pending(self, signal, 0, TRUE)) return;
      g_autoptr(EphyEngineView) keep_alive = EPHY_ENGINE_VIEW(g_object_ref(self));
      g_autoptr(EphyEngineResponse) native_response = ephy_engine_response_new(uri, response);
      g_signal_emit_by_name(self, "resource-response", uri.c_str(), native_response);
    };
    browser->on_load_end = [this](int) {
      probe_extension_context();
      if (browser->cef_browser() && browser->url.starts_with("chrome-extension://") && !browser->is_extension_popup && browser->native_tab_id < 0) {
        auto args = CefDictionaryValue::Create();
        args->SetString("expression", "new Promise(resolve => chrome.tabs.getCurrent(tab => { const error = chrome.runtime.lastError; resolve(error ? -1 : tab?.id ?? -1); }))");
        args->SetBool("awaitPromise", true); args->SetBool("returnByValue", true);
        auto document = std::shared_ptr<EphyEngineResource>(static_cast<EphyEngineResource *>(g_object_ref(resource)),
          [](auto *resource) { g_object_unref(resource); });
        const auto epoch = protocol->document_epoch, generation = extension_context_generation;
        const int browser_id = browser->cef_browser()->GetIdentifier();
        protocol->send("Runtime.evaluate", args, [document, epoch, generation, browser_id](auto reply, const auto& error) {
          g_autoptr(EphyEngineView) view = EPHY_ENGINE_VIEW(g_weak_ref_get(&document->view));
          if (!view || !error.empty() || !reply) return;
          auto *current = data(view);
          if (current->resource != document.get() || !current->extension_context_current(epoch, generation, browser_id)) return;
          auto result = reply->GetDictionary("result");
          if (result && result->GetString("type") == "number" && result->GetInt("value") >= 0)
            current->browser->native_tab_id = result->GetInt("value");
        });
      }
      update_history();
    };
    browser->on_load_error = [this](const std::string& url, const std::string& message) {
      if (disposed || detached) return false;
      g_autoptr(EphyEngineView) keep_alive = EPHY_ENGINE_VIEW(g_object_ref(self));
      g_autoptr(GError) error = g_error_new_literal(G_IO_ERROR, G_IO_ERROR_FAILED, message.c_str());
      gboolean handled = false;
      g_signal_emit_by_name(self, "load-failed", WEBKIT_LOAD_STARTED, url.c_str(), error, &handled);
      return bool(handled);
    };
    browser->on_tls_error = [this](const std::string& url, CefRefPtr<CefSSLInfo> info) {
      if (disposed || detached || !info) return false;
      auto certificate = info->GetX509Certificate();
      g_autoptr(GTlsCertificate) tls = ephy_engine_certificate_to_tls(certificate);
      if (!tls) return false;
      auto errors = ephy_engine_certificate_errors(info->GetCertStatus(), certificate);
      if (!errors) errors = G_TLS_CERTIFICATE_GENERIC_ERROR;
      g_autoptr(EphyEngineView) keep_alive = EPHY_ENGINE_VIEW(g_object_ref(self));
      gboolean handled = false;
      g_signal_emit_by_name(self, "load-failed-with-tls-errors", url.c_str(), tls, errors, &handled);
      return bool(handled);
    };
    host.open_tab = [this](const std::string& url, bool active, bool defer) -> CefRefPtr<Browser> {
      g_autoptr(EphyEngineView) owner = browser->is_extension_popup ?
        EPHY_ENGINE_VIEW(g_weak_ref_get(&extension_owner)) : EPHY_ENGINE_VIEW(g_object_ref(self));
      if (!owner || data(owner)->disposed || (browser->is_extension_popup && (data(owner)->host.private_mode || !gtk_widget_get_root(GTK_WIDGET(owner))))) return nullptr;
      GtkWidget* view = nullptr;
      g_signal_emit_by_name(owner, "open-link", url.c_str(), active, defer, &view);
      if (!view || !EPHY_IS_ENGINE_VIEW(view)) return nullptr;
      return data(EPHY_ENGINE_VIEW(view))->browser;
    };
    gtk_widget_set_parent(browser->widget(), GTK_WIDGET(self));
    views.push_back(this);
  }
  static ViewData* data(EphyEngineView*);
  const std::string& visible_uri(const std::string& url) const {
    auto found = alternate_uris.find(url);
    return found == alternate_uris.end() ? url : found->second;
  }
  void cancel_external_uri() {
    g_autoptr(GCancellable) pending = external_uri;
    external_uri = nullptr;
    if (pending) g_cancellable_cancel(pending);
  }
  void stop_extension_context_probe() {
    if (extension_context_timer) { g_source_remove(extension_context_timer); extension_context_timer = 0; }
  }
  bool extension_context_current(guint64 epoch, guint64 generation, int browser_id) {
    if (disposed || detached || host.private_mode || browser->closing || protocol->document_epoch != epoch ||
        extension_context_generation != generation ||
        !browser->cef_browser() || browser->cef_browser()->GetIdentifier() != browser_id ||
        !gtk_widget_get_root(GTK_WIDGET(self))) return false;
    if (browser->is_extension_popup) {
      g_autoptr(EphyEngineView) owner = EPHY_ENGINE_VIEW(g_weak_ref_get(&extension_owner));
      if (!owner || data(owner)->disposed || data(owner)->host.private_mode ||
          !gtk_widget_get_root(GTK_WIDGET(owner))) return false;
    }
    return true;
  }
  void probe_extension_context() {
    stop_extension_context_probe();
    if (extension_context_reload_used || !browser->url.starts_with("chrome-extension://") ||
        !browser->cef_browser() || !runtime) return;
    const auto id = browser->url.substr(19, browser->url.find('/', 19) - 19);
    if (!runtime->extension_host->extension_ids.contains(id)) return;
    struct Probe {
      GWeakRef view;
      guint64 epoch, generation;
      gint64 deadline = g_get_monotonic_time() + 5 * G_TIME_SPAN_SECOND;
      int browser;
      unsigned checks = 0;
      bool pending = false;
      Probe(ViewData *owner) : epoch(owner->protocol->document_epoch), generation(owner->extension_context_generation), browser(owner->browser->cef_browser()->GetIdentifier()) {
        g_weak_ref_init(&view, owner->self);
      }
      ~Probe() { g_weak_ref_clear(&view); }
    };
    auto state = std::make_shared<Probe>(this);
    extension_context_timer = g_timeout_add_full(G_PRIORITY_DEFAULT, 250, +[](gpointer pointer) -> gboolean {
      auto state = *static_cast<std::shared_ptr<Probe> *>(pointer);
      g_autoptr(EphyEngineView) view = EPHY_ENGINE_VIEW(g_weak_ref_get(&state->view));
      if (!view) return G_SOURCE_REMOVE;
      auto *current = data(view);
      if (!current->extension_context_current(state->epoch, state->generation, state->browser) || state->checks >= 20 || g_get_monotonic_time() > state->deadline) {
        current->extension_context_timer = 0;
        return G_SOURCE_REMOVE;
      }
      if (state->pending) return G_SOURCE_CONTINUE;
      state->pending = true; ++state->checks;
      auto args = CefDictionaryValue::Create();
      args->SetString("expression", R"JS((()=>{try {
        if (typeof chrome?.runtime?.getURL !== 'function' || typeof chrome.runtime.getManifest !== 'function') return false;
        chrome.runtime.getURL(''); chrome.runtime.getManifest(); return false;
      } catch(error) { return error?.message === 'Extension context invalidated.'; }})())JS");
      args->SetBool("returnByValue", true);
      current->protocol->send("Runtime.evaluate", args, [state](auto reply, const auto& error) {
        state->pending = false;
        g_autoptr(EphyEngineView) view = EPHY_ENGINE_VIEW(g_weak_ref_get(&state->view));
        if (!view) return;
        auto *current = data(view);
        if (!current->extension_context_current(state->epoch, state->generation, state->browser) || current->extension_context_reload_used ||
            !error.empty() || !reply || reply->HasKey("exceptionDetails")) return;
        auto result = reply->GetDictionary("result");
        if (!result || result->GetString("type") != "boolean" || !result->GetBool("value")) return;
        // Extension self-reloads invalidate existing native bindings; one reload reconnects the document.
        current->extension_context_reload_used = true;
        if (runtime && runtime->render_test) g_print("CEF explicit native extension context invalidation: reloading once\n");
        current->stop_extension_context_probe();
        current->browser->reload();
      });
      return G_SOURCE_CONTINUE;
    }, new std::shared_ptr<Probe>(std::move(state)), +[](gpointer pointer) {
      delete static_cast<std::shared_ptr<Probe> *>(pointer);
    });
  }
  void apply_user_agent() {
    if (!protocol->browser || disposed) return;
    g_autofree char *agent = has_user_agent_override ? g_strdup(user_agent_override.c_str()) : ephy_engine_settings_dup_user_agent();
    auto args = CefDictionaryValue::Create(); args->SetString("userAgent", agent ? agent : "");
    // Chromium applies the network override synchronously; its renderer reply needs a document.
    protocol->send("Emulation.setUserAgentOverride", args, [this](auto, const auto& error) {
      if (disposed) return;
      if (!error.empty()) g_warning("Could not apply Chromium user agent: %s", error.c_str());
    });
  }
  void changed() {
    if (disposed) return;
    if (auto page = browser->brave_page(); page && protocol->brave.expired()) {
      protocol->brave = page;
      page->zoom_changed = [this](double factor) {
        if (!disposed && !detached && zoom != factor) {
          zoom = factor; g_object_notify(G_OBJECT(self),"zoom-level");
        }
      };
      page->set_zoom(zoom);
      page->download_begin = [this](const std::string& uri, const std::string& name, const std::string& staging,
                                     std::function<void()> cancel) -> GnosisBrave::DownloadProgress {
        if (disposed || detached) return {};
        auto* download = ephy_engine_download_native_begin(host.session ? host.session : ephy_engine_session_get_default(),
          uri, name, staging, std::move(cancel));
        auto keep = std::shared_ptr<EphyEngineDownload>(download, [](auto* value) { g_object_unref(value); });
        return [keep](guint64 received, guint64 total, const std::string& state) {
          ephy_engine_download_native_update(keep.get(), received, total, state);
        };
      };
      page->internal_resource = [this](const std::string& uri, bool main, GnosisBrave::ResourceReply reply) {
        if (disposed || detached) { reply(nullptr, "", "The view is closed"); return; }
        ephy_engine_uri_scheme_fetch_async(self, uri.c_str(), main, nullptr,
          +[](GObject*, GAsyncResult* result, gpointer user) {
            std::unique_ptr<GnosisBrave::ResourceReply> reply(static_cast<GnosisBrave::ResourceReply*>(user));
            g_autoptr(GError) error = nullptr; g_autofree char* mime = nullptr;
            g_autoptr(GBytes) bytes = ephy_engine_uri_scheme_fetch_finish(result, &mime, &error);
            (*reply)(bytes, mime ? mime : "application/octet-stream", error ? error->message : "");
          }, new GnosisBrave::ResourceReply(std::move(reply)));
      };
      page->page_message = [this](const std::string& uri, const std::string& name, const std::string& payload) {
        if (!disposed && !detached && browser->on_page_message) browser->on_page_message(uri, name, payload);
      };
      auto current = protocol;
      page->event = [current](const std::string& method, JsonObject* params) {
        auto* node = json_node_new(JSON_NODE_OBJECT); json_node_set_object(node, params);
        char* json = json_to_string(node, false);
        current->OnDevToolsEvent(nullptr, method, json, strlen(json));
        g_free(json); json_node_free(node);
      };
    }
    if (browser->cef_browser() && !protocol->browser) {
      protocol->browser = browser->cef_browser();
      protocol->registration = protocol->browser->GetHost()->AddDevToolsMessageObserver(protocol);
      protocol->send("Page.enable", nullptr);
      protocol->send("Runtime.enable", nullptr);
      auto current = protocol;
      protocol->send("Page.getFrameTree", nullptr, [current](auto reply, const auto&) {
        auto tree = reply ? reply->GetDictionary("frameTree") : nullptr;
        auto frame = tree ? tree->GetDictionary("frame") : nullptr;
        if (frame) current->main_frame = frame->GetString("id");
      });
    }
    if (uri != visible_uri(browser->url)) { uri = visible_uri(browser->url); g_object_notify(G_OBJECT(self), "uri"); }
    if (title != browser->name) { title = browser->name; g_object_notify(G_OBJECT(self), "title"); }
    if (loading != browser->loading) { loading = browser->loading; g_object_notify(G_OBJECT(self), "is-loading"); }
    if (progress != browser->progress) { progress = browser->progress; g_object_notify(G_OBJECT(self), "estimated-load-progress"); }
    g_object_notify(G_OBJECT(self), "can-go-back"); g_object_notify(G_OBJECT(self), "can-go-forward");
  }
  void update_history(bool load_events = true) {
    auto expected = std::shared_ptr<EphyEngineResource>(static_cast<EphyEngineResource*>(g_object_ref(resource)),
      [](auto *resource) { g_object_unref(resource); });
    protocol->send("Page.getNavigationHistory", nullptr, [this, expected, load_events](auto reply, const auto& error) {
      if (disposed || resource != expected.get()) return;
      g_autoptr(EphyEngineView) keep_alive = EPHY_ENGINE_VIEW(g_object_ref(self));
      auto entry = browser->cef_browser() ? browser->cef_browser()->GetHost()->GetVisibleNavigationEntry() : nullptr;
      const bool restored = entry && (entry->GetTransitionType() & TT_FORWARD_BACK_FLAG);
      if (error.empty() && reply && reply->HasKey("entries")) {
        for (auto* item : *history->items) g_object_unref(item);
        history->items->clear();
        history->current = reply->GetInt("currentIndex");
        auto entries = reply->GetList("entries");
        std::set<int> ids;
        for (size_t i = 0; i < entries->GetSize(); i++) {
          auto entry = entries->GetDictionary(i);
          auto* item = reinterpret_cast<EphyEngineHistoryItem*>(g_object_new(ephy_engine_history_item_get_type(), nullptr));
          auto url = entry->GetString("url").ToString();
          if (auto page = browser->brave_page()) url = page->canonical_uri(url);
          item->uri = g_strdup(visible_uri(url).c_str());
          item->alternate = alternate_uris.contains(url);
          item->title = g_strdup(entry->GetString("title").ToString().c_str());
          item->id = entry->GetInt("id"); history->items->push_back(item);
          ids.insert(item->id);
          if (static_cast<int>(i) == history->current) {
            if (resource->response) {
              auto& cached = history_responses[item->id];
              g_set_object(&cached, resource->response);
            } else if (auto found = history_responses.find(item->id); restored && found != history_responses.end()) {
              // Restoring from Chromium's page cache does not run network callbacks.
              g_set_object(&resource->response, found->second);
              g_free(resource->uri); resource->uri = g_strdup(ephy_engine_response_get_uri(found->second));
            }
          }
        }
        for (auto it = history_responses.begin(); it != history_responses.end();) {
          if (ids.contains(it->first)) ++it;
          else { g_object_unref(it->second); it = history_responses.erase(it); }
        }
        g_signal_emit_by_name(history, "changed", nullptr, nullptr);
      }
      if (load_events) {
        g_signal_emit_by_name(resource, "finished");
        g_signal_emit_by_name(self, "load-changed", WEBKIT_LOAD_COMMITTED);
        g_signal_emit_by_name(self, "load-changed", WEBKIT_LOAD_FINISHED);
      }
    });
  }
  ~ViewData() {
    cancel_external_uri();
    stop_extension_context_probe();
    std::erase(views, this);
    g_weak_ref_clear(&extension_owner);
    protocol->stop();
    for (const auto& [id, response] : history_responses) g_object_unref(response);
    for (GObject* object : {G_OBJECT(history), G_OBJECT(find), G_OBJECT(inspector), G_OBJECT(resource)}) g_object_unref(object);
    g_clear_object(&session); g_clear_object(&ucm); g_clear_object(&settings);
    g_clear_object(&favicon);
  }
};
}
typedef struct { ViewData* data; } EphyEngineViewPrivate;
G_DEFINE_TYPE_WITH_PRIVATE(EphyEngineView, ephy_engine_web_view, GTK_TYPE_WIDGET)
ViewData* ViewData::data(EphyEngineView* view) { return static_cast<EphyEngineViewPrivate*>(ephy_engine_web_view_get_instance_private(view))->data; }
namespace {
ViewData* data(EphyEngineView* view) { return ViewData::data(view); }
enum { P_NONE, P_TITLE, P_URI, P_LOADING, P_PROGRESS, P_ZOOM, P_MUTED, P_AUDIO, P_FAVICON, P_BACK, P_FORWARD,
 P_RESPONSIVE, P_SESSION, P_UCM, P_SETTINGS, P_RELATED, P_AUTOMATION, P_EXTENSION_MODE, P_CSP };
void unsupported(GTask* task, const char* message) {
  g_task_return_new_error(task, G_IO_ERROR, G_IO_ERROR_NOT_SUPPORTED, "%s", message); g_object_unref(task);
}
void set_object(GObject** slot, const GValue* value) { g_set_object(slot, G_OBJECT(g_value_get_object(value))); }
}
static void ephy_engine_web_view_class_init(EphyEngineViewClass* klass) {
  auto* object_class = G_OBJECT_CLASS(klass); auto* widget_class = GTK_WIDGET_CLASS(klass);
  widget_class->grab_focus = +[](GtkWidget* widget) -> gboolean { data(EPHY_ENGINE_VIEW(widget))->browser->focus_native(); return true; };
  widget_class->root = +[](GtkWidget *widget) {
    data(EPHY_ENGINE_VIEW(widget))->detached = false;
    GTK_WIDGET_CLASS(ephy_engine_web_view_parent_class)->root(widget);
  };
  widget_class->unroot = +[](GtkWidget *widget) {
    auto *view = data(EPHY_ENGINE_VIEW(widget));
    view->detached = true;
    view->cancel_external_uri();
    view->stop_extension_context_probe();
    view->browser->cancel_permissions();
    view->browser->cancel_authentication();
    // Pending GTasks retain their source widget, so disposal alone cannot cancel them.
    view->protocol->cancel_pending("Tab detached from its window");
    GTK_WIDGET_CLASS(ephy_engine_web_view_parent_class)->unroot(widget);
  };
  widget_class->unmap = +[](GtkWidget *widget) {
    data(EPHY_ENGINE_VIEW(widget))->cancel_external_uri();
    data(EPHY_ENGINE_VIEW(widget))->browser->cancel_authentication();
    GTK_WIDGET_CLASS(ephy_engine_web_view_parent_class)->unmap(widget);
  };
  gtk_widget_class_set_layout_manager_type(widget_class, GTK_TYPE_BIN_LAYOUT);
  object_class->dispose = +[](GObject* object) {
    auto* d = data(EPHY_ENGINE_VIEW(object));
    if (d && !d->disposed) {
      d->cancel_external_uri();
      d->disposed = true; d->stop_extension_context_probe(); d->protocol->stop(); d->browser->detach_callbacks(); d->browser->close(); d->browser->host = nullptr;
      if (gtk_widget_get_parent(d->browser->widget())) gtk_widget_unparent(d->browser->widget());
    }
    G_OBJECT_CLASS(ephy_engine_web_view_parent_class)->dispose(object);
  };
  object_class->finalize = +[](GObject* object) { delete data(EPHY_ENGINE_VIEW(object)); G_OBJECT_CLASS(ephy_engine_web_view_parent_class)->finalize(object); };
  object_class->get_property = +[](GObject* object, guint id, GValue* value, GParamSpec* spec) {
    auto* d = data(EPHY_ENGINE_VIEW(object));
    switch(id) {
    case P_TITLE: g_value_set_string(value, d->title.c_str()); break;
    case P_URI: g_value_set_string(value, d->uri.c_str()); break;
    case P_LOADING: g_value_set_boolean(value, d->loading); break;
    case P_PROGRESS: g_value_set_double(value, d->progress); break;
    case P_ZOOM: g_value_set_double(value, d->zoom); break;
    case P_MUTED: g_value_set_boolean(value, d->muted); break;
    case P_AUDIO: g_value_set_boolean(value, false); break;
    case P_RESPONSIVE: g_value_set_boolean(value, true); break;
    case P_BACK: g_value_set_boolean(value, d->browser->can_back()); break;
    case P_FORWARD: g_value_set_boolean(value, d->browser->can_forward()); break;
    case P_FAVICON: g_value_set_object(value, d->favicon); break;
    case P_SESSION: g_value_set_object(value, d->session); break;
    case P_UCM: g_value_set_object(value, d->ucm); break;
    case P_SETTINGS: g_value_set_object(value, d->settings); break;
    case P_RELATED: g_value_set_object(value, nullptr); break;
    case P_AUTOMATION: g_value_set_boolean(value, false); break;
    case P_EXTENSION_MODE: g_value_set_enum(value, WEBKIT_WEB_EXTENSION_MODE_NONE); break;
    case P_CSP: g_value_set_string(value, nullptr); break;
    default: G_OBJECT_WARN_INVALID_PROPERTY_ID(object, id, spec);
    }
  };
  object_class->set_property = +[](GObject* object, guint id, const GValue* value, GParamSpec* spec) {
    auto* d = data(EPHY_ENGINE_VIEW(object));
    switch(id) {
    case P_SESSION: {
      auto* session = EPHY_ENGINE_SESSION(g_value_get_object(value));
      g_set_object(reinterpret_cast<GObject**>(&d->session), G_OBJECT(session ? session : ephy_engine_session_get_default()));
      d->host.session = d->session;
      d->host.private_mode = ephy_engine_session_is_ephemeral(d->session);
      d->host.context = ephy_engine_session_context(d->session);
      break;
    }
    case P_UCM: set_object(reinterpret_cast<GObject**>(&d->ucm), value); break;
    case P_SETTINGS: {
      auto *settings = EPHY_ENGINE_SETTINGS(g_value_get_object(value));
      g_set_object(reinterpret_cast<GObject**>(&d->settings), G_OBJECT(settings ? settings : ephy_engine_settings_get_default()));
      break;
    }
    case P_RELATED: {
      auto* related = EPHY_ENGINE_VIEW(g_value_get_object(value));
      if (related) {
        g_set_object(reinterpret_cast<GObject**>(&d->session), G_OBJECT(data(related)->session));
        d->host.session = d->session;
        d->host.context = data(related)->host.context;
        d->host.private_mode = data(related)->host.private_mode;
        d->browser->defer_start = data(related)->creating_popup;
      }
      break;
    }
    case P_AUTOMATION: case P_EXTENSION_MODE: case P_CSP: break;
    default: G_OBJECT_WARN_INVALID_PROPERTY_ID(object, id, spec);
    }
  };
  auto flags = GParamFlags(G_PARAM_READABLE | G_PARAM_STATIC_STRINGS);
  auto construct = GParamFlags(G_PARAM_READWRITE | G_PARAM_CONSTRUCT_ONLY | G_PARAM_STATIC_STRINGS);
  for (auto [id, name] : {std::pair{P_TITLE,"title"}, {P_URI,"uri"}})
    g_object_class_install_property(object_class, id, g_param_spec_string(name, nullptr, nullptr, "", flags));
  for (auto [id, name] : {std::pair{P_LOADING,"is-loading"}, {P_MUTED,"is-muted"}, {P_AUDIO,"is-playing-audio"}, {P_BACK,"can-go-back"}, {P_FORWARD,"can-go-forward"}, {P_RESPONSIVE,"is-web-process-responsive"}})
    g_object_class_install_property(object_class, id, g_param_spec_boolean(name, nullptr, nullptr, id==P_RESPONSIVE, flags));
  g_object_class_install_property(object_class, P_PROGRESS, g_param_spec_double("estimated-load-progress", nullptr, nullptr, 0, 1, 0, flags));
  g_object_class_install_property(object_class, P_ZOOM, g_param_spec_double("zoom-level", nullptr, nullptr, .1, 10, 1, flags));
  g_object_class_install_property(object_class, P_FAVICON, g_param_spec_object("favicon", nullptr, nullptr, GDK_TYPE_TEXTURE, flags));
  for (auto [id, name, type] : {std::tuple{P_SESSION,"network-session",EPHY_TYPE_ENGINE_SESSION}, {P_UCM,"user-content-manager",WEBKIT_TYPE_USER_CONTENT_MANAGER}, {P_SETTINGS,"settings",EPHY_TYPE_ENGINE_SETTINGS}, {P_RELATED,"related-view",EPHY_TYPE_ENGINE_VIEW}})
    g_object_class_install_property(object_class, id, g_param_spec_object(name, nullptr, nullptr, type, construct));
  g_object_class_install_property(object_class, P_AUTOMATION, g_param_spec_boolean("is-controlled-by-automation", nullptr, nullptr, false, construct));
  g_object_class_install_property(object_class, P_EXTENSION_MODE, g_param_spec_enum("web-extension-mode", nullptr, nullptr, WEBKIT_TYPE_WEB_EXTENSION_MODE, WEBKIT_WEB_EXTENSION_MODE_NONE, construct));
  g_object_class_install_property(object_class, P_CSP, g_param_spec_string("default-content-security-policy", nullptr, nullptr, nullptr, construct));
  g_signal_new("open-link", G_TYPE_FROM_CLASS(klass), G_SIGNAL_RUN_LAST, 0, nullptr, nullptr, nullptr, GTK_TYPE_WIDGET, 3, G_TYPE_STRING, G_TYPE_BOOLEAN, G_TYPE_BOOLEAN);
  g_signal_new("load-changed", G_TYPE_FROM_CLASS(klass), G_SIGNAL_RUN_LAST, G_STRUCT_OFFSET(EphyEngineViewClass, load_changed), nullptr, nullptr, nullptr, G_TYPE_NONE, 1, WEBKIT_TYPE_LOAD_EVENT);
  g_signal_new("load-failed", G_TYPE_FROM_CLASS(klass), G_SIGNAL_RUN_LAST, G_STRUCT_OFFSET(EphyEngineViewClass, load_failed), g_signal_accumulator_true_handled, nullptr, nullptr, G_TYPE_BOOLEAN, 3, WEBKIT_TYPE_LOAD_EVENT, G_TYPE_STRING, G_TYPE_ERROR);
  g_signal_new("create-popup", G_TYPE_FROM_CLASS(klass), G_SIGNAL_RUN_LAST, G_STRUCT_OFFSET(EphyEngineViewClass, create_popup), g_signal_accumulator_first_wins, nullptr, nullptr, GTK_TYPE_WIDGET, 1, G_TYPE_POINTER);
  g_signal_new("popup-geometry-changed", G_TYPE_FROM_CLASS(klass), G_SIGNAL_RUN_LAST, 0, nullptr, nullptr, nullptr, G_TYPE_NONE, 0);
  g_signal_new("extension-popup-size-changed", G_TYPE_FROM_CLASS(klass), G_SIGNAL_RUN_LAST, 0, nullptr, nullptr, nullptr, G_TYPE_NONE, 0);
  g_signal_new("ready-to-show", G_TYPE_FROM_CLASS(klass), G_SIGNAL_RUN_LAST, G_STRUCT_OFFSET(EphyEngineViewClass, ready_to_show), nullptr, nullptr, nullptr, G_TYPE_NONE, 0);
  g_signal_new("tls-info-changed", G_TYPE_FROM_CLASS(klass), G_SIGNAL_RUN_LAST, 0, nullptr, nullptr, nullptr, G_TYPE_NONE, 0);
  g_signal_new("run-as-modal", G_TYPE_FROM_CLASS(klass), G_SIGNAL_RUN_LAST, G_STRUCT_OFFSET(EphyEngineViewClass, run_as_modal), nullptr, nullptr, nullptr, G_TYPE_NONE, 0);
  g_signal_new("close", G_TYPE_FROM_CLASS(klass), G_SIGNAL_RUN_LAST, G_STRUCT_OFFSET(EphyEngineViewClass, close), nullptr, nullptr, nullptr, G_TYPE_NONE, 0);
  g_signal_new("permission-request", G_TYPE_FROM_CLASS(klass), G_SIGNAL_RUN_LAST, G_STRUCT_OFFSET(EphyEngineViewClass, permission_request), g_signal_accumulator_true_handled, nullptr, nullptr, G_TYPE_BOOLEAN, 1, EPHY_TYPE_ENGINE_PERMISSION);
  g_signal_new("enter-fullscreen", G_TYPE_FROM_CLASS(klass), G_SIGNAL_RUN_LAST, G_STRUCT_OFFSET(EphyEngineViewClass, enter_fullscreen), g_signal_accumulator_true_handled, nullptr, nullptr, G_TYPE_BOOLEAN, 0);
  g_signal_new("leave-fullscreen", G_TYPE_FROM_CLASS(klass), G_SIGNAL_RUN_LAST, G_STRUCT_OFFSET(EphyEngineViewClass, leave_fullscreen), g_signal_accumulator_true_handled, nullptr, nullptr, G_TYPE_BOOLEAN, 0);
  g_signal_new("load-failed-with-tls-errors", G_TYPE_FROM_CLASS(klass), G_SIGNAL_RUN_LAST, G_STRUCT_OFFSET(EphyEngineViewClass, load_failed_with_tls_errors), g_signal_accumulator_true_handled, nullptr, nullptr, G_TYPE_BOOLEAN, 3, G_TYPE_STRING, G_TYPE_TLS_CERTIFICATE, G_TYPE_TLS_CERTIFICATE_FLAGS);
  g_signal_new("page-message", G_TYPE_FROM_CLASS(klass), static_cast<GSignalFlags>(G_SIGNAL_RUN_LAST | G_SIGNAL_DETAILED),
               0, nullptr, nullptr, nullptr, G_TYPE_NONE, 1, JSON_TYPE_NODE);
  g_signal_new("resource-response", G_TYPE_FROM_CLASS(klass), G_SIGNAL_RUN_LAST, 0,
               nullptr, nullptr, nullptr, G_TYPE_NONE, 2, G_TYPE_STRING, EPHY_TYPE_ENGINE_RESPONSE);
  g_signal_new("authentication-request", G_TYPE_FROM_CLASS(klass), G_SIGNAL_RUN_LAST, 0,
               g_signal_accumulator_true_handled, nullptr, nullptr, G_TYPE_BOOLEAN, 1,
               EPHY_TYPE_ENGINE_AUTHENTICATION);
  g_signal_new("client-certificate-request", G_TYPE_FROM_CLASS(klass), G_SIGNAL_RUN_LAST, 0,
               g_signal_accumulator_true_handled, nullptr, nullptr, G_TYPE_BOOLEAN, 1,
               EPHY_TYPE_ENGINE_CLIENT_CERTIFICATE);
  g_signal_new("webstore-install-blocked", G_TYPE_FROM_CLASS(klass), G_SIGNAL_RUN_LAST, 0,
               nullptr, nullptr, nullptr, G_TYPE_NONE, 0);
  g_signal_new("open-external-uri", G_TYPE_FROM_CLASS(klass), G_SIGNAL_RUN_LAST, 0,
               g_signal_accumulator_true_handled, nullptr, nullptr, G_TYPE_BOOLEAN, 4,
               G_TYPE_STRING, G_TYPE_STRING, G_TYPE_BOOLEAN, G_TYPE_CANCELLABLE);
}
static void ephy_engine_web_view_init(EphyEngineView* view) {
  static_cast<EphyEngineViewPrivate*>(ephy_engine_web_view_get_instance_private(view))->data = new ViewData(view);
}

namespace {
class Engine final : public CefApp, public CefBrowserProcessHandler, public CefResourceBundleHandler {
public:
  std::string extension_paths;
  bool diagnostic = false;
  CefRefPtr<CefBrowserProcessHandler> GetBrowserProcessHandler() override { return this; }
  CefRefPtr<CefResourceBundleHandler> GetResourceBundleHandler() override { return this; }
  bool GetLocalizedString(int id, CefString& value) override {
    if (id != IDS_PRODUCT_NAME) return false;
    value = "Gnosis";
    return true;
  }
  bool GetDataResource(int, void*&, size_t&) override { return false; }
  bool GetDataResourceForScale(int, ScaleFactor, void*&, size_t&) override { return false; }
  CefRefPtr<CefRenderProcessHandler> GetRenderProcessHandler() override { return renderer_; }
  void OnBeforeCommandLineProcessing(const CefString& process, CefRefPtr<CefCommandLine> command) override {
    if (!process.empty()) return;
    configure_extensions(command, extension_paths);
    command->AppendSwitch("disable-chrome-login-prompt");
    if (diagnostic) command->AppendSwitchWithValue("password-store", "basic");
    command->AppendSwitchWithValue("ozone-platform", g_getenv("WAYLAND_DISPLAY") ? "wayland" : "x11");
    // Dawn needs Chromium's Vulkan context on Linux; keep its adapter blocklist.
    auto features = command->GetSwitchValue("enable-features").ToString();
    command->AppendSwitchWithValue("enable-features", features + ",WebGPUService,Vulkan,VulkanFromANGLE");
    if (!command->HasSwitch("use-angle")) command->AppendSwitchWithValue("use-angle", "vulkan");
    ephy_engine_settings_configure_command_line(command);
  }
  void OnRegisterCustomSchemes(CefRawPtr<CefSchemeRegistrar> registrar) override { ephy_engine_register_schemes(registrar); }
  void OnRegisterCustomPreferences(cef_preferences_type_t type, CefRawPtr<CefPreferenceRegistrar> registrar) override {
    ephy_engine_sessions_register_preferences(type, registrar);
  }
  void OnContextInitialized() override { ephy_engine_sessions_initialize(); enable_extension_developer_mode(); ephy_engine_install_scheme_handlers(); }
private:
  CefRefPtr<CefRenderProcessHandler> renderer_ = PageMessages::renderer_handler();
  IMPLEMENT_REFCOUNTING(Engine);
};
CefRefPtr<Engine> engine;
}
char* ephy_engine_dup_version_info() {
  cef_version_info_t version{};
  version.size = sizeof(version);
  cef_version_info_all(&version);
  return g_strdup_printf("CEF %d.%d.%d\nChromium %d.%d.%d.%d",
    version.cef_version_major, version.cef_version_minor, version.cef_version_patch,
    version.chrome_version_major, version.chrome_version_minor,
    version.chrome_version_build, version.chrome_version_patch);
}
JsonArray *ephy_engine_extensions_dup_installed(void) {
  if (GnosisBrave::enabled()) return ephy_engine_extensions_dup_native_snapshot();
  auto *catalog = json_array_new();
  if (!runtime) return catalog;
  for (const auto& extension : runtime->extensions.installed) {
    auto *item = json_object_new();
    for (const auto& [key, value] : {std::pair{"id", extension.id}, {"name", extension.name},
         {"version", extension.version}, {"path", extension.path}, {"popup", extension.popup},
         {"options", extension.options}}) json_object_set_string_member(item, key, value.c_str());
    json_object_set_boolean_member(item, "enabled", extension.enabled && !extension.restart_required);
    json_object_set_boolean_member(item, "restart_required", extension.restart_required);
    json_object_set_boolean_member(item, "builtin", extension.builtin);
    json_object_set_int_member(item, "manifest_version", extension.manifest);
    json_object_set_boolean_member(item, "has_action", false);
    for (auto *key : {"description", "author", "homepage", "icon"}) json_object_set_string_member(item, key, "");
    g_autoptr(JsonParser) parser = json_parser_new();
    if (json_parser_load_from_file(parser, (extension.path + "/manifest.json").c_str(), nullptr) &&
        JSON_NODE_HOLDS_OBJECT(json_parser_get_root(parser))) {
      auto *manifest = json_node_get_object(json_parser_get_root(parser));
      for (auto *key : {"description", "author", "homepage_url"}) {
        auto *node = json_object_get_member(manifest, key);
        if (node && JSON_NODE_HOLDS_VALUE(node) && json_node_get_value_type(node) == G_TYPE_STRING) {
          std::string value = json_node_get_string(node);
          if (value.starts_with("__MSG_") && value.ends_with("__")) {
            auto token = value.substr(6, value.size() - 8);
            value.clear();
            auto *locale = json_object_get_member(manifest, "default_locale");
            if (locale && JSON_NODE_HOLDS_VALUE(locale) && json_node_get_value_type(locale) == G_TYPE_STRING) {
              std::string name = json_node_get_string(locale);
              g_autoptr(JsonParser) messages = json_parser_new();
              if (!name.empty() && std::all_of(name.begin(), name.end(), [](char c) { return g_ascii_isalnum(c) || c == '_' || c == '-'; }) &&
                  json_parser_load_from_file(messages, (extension.path + "/_locales/" + name + "/messages.json").c_str(), nullptr) &&
                  JSON_NODE_HOLDS_OBJECT(json_parser_get_root(messages))) {
                auto *message = json_object_get_member(json_node_get_object(json_parser_get_root(messages)), token.c_str());
                auto *text = message && JSON_NODE_HOLDS_OBJECT(message) ? json_object_get_member(json_node_get_object(message), "message") : nullptr;
                if (text && JSON_NODE_HOLDS_VALUE(text) && json_node_get_value_type(text) == G_TYPE_STRING) value = json_node_get_string(text);
              }
            }
          }
          json_object_set_string_member(item, !strcmp(key, "homepage_url") ? "homepage" : key, value.c_str());
        }
      }
      for (auto *key : {"action", "browser_action", "page_action"}) {
        auto *node = json_object_get_member(manifest, key);
        if (node && JSON_NODE_HOLDS_OBJECT(node)) json_object_set_boolean_member(item, "has_action", true);
      }
      auto *icons_node = json_object_get_member(manifest, "icons");
      if (icons_node && JSON_NODE_HOLDS_OBJECT(icons_node)) {
        auto *icons = json_node_get_object(icons_node);
        g_autoptr(GList) keys = json_object_get_members(icons);
        std::string best;
        int distance = G_MAXINT;
        for (auto *key = keys; key; key = key->next) {
          auto *name = static_cast<const char *>(key->data);
          auto *node = json_object_get_member(icons, name);
          auto size = g_ascii_strtoll(name, nullptr, 10);
          if (!node || !JSON_NODE_HOLDS_VALUE(node) || json_node_get_value_type(node) != G_TYPE_STRING ||
              size < 1 || size > 4096 || std::abs(size - 48) >= distance) continue;
          std::error_code error;
          auto base = std::filesystem::weakly_canonical(extension.path, error).string();
          if (error) continue;
          auto path = std::filesystem::weakly_canonical(std::filesystem::path(base) / json_node_get_string(node), error).string();
          if (error || !path.starts_with(base + "/") || !g_file_test(path.c_str(), G_FILE_TEST_IS_REGULAR)) continue;
          best = path; distance = std::abs(size - 48);
        }
        json_object_set_string_member(item, "icon", best.c_str());
      }
    }
    json_array_add_object_element(catalog, item);
  }
  return catalog;
}
gint ephy_engine_web_view_get_extension_tab_id(EphyEngineView *view) {
  g_return_val_if_fail(EPHY_IS_ENGINE_VIEW(view), -1);
  auto *d = data(view);
  if (d->disposed || d->host.private_mode || d->browser->is_extension_popup || d->browser->closing) return -1;
  if (auto page = d->browser->brave_page()) return page->native_tab_id();
  return d->browser->native_tab_id;
}
gboolean ephy_engine_web_view_set_extension_popup(EphyEngineView *popup, EphyEngineView *owner) {
  g_return_val_if_fail(EPHY_IS_ENGINE_VIEW(popup), false);
  g_return_val_if_fail(EPHY_IS_ENGINE_VIEW(owner), false);
  auto *view = data(popup), *parent = data(owner);
  if (view == parent || view->disposed || parent->disposed || view->host.private_mode ||
      parent->host.private_mode || parent->browser->is_extension_popup || view->browser->loaded) return false;
  if (auto page = parent->browser->brave_page()) {
    const int tab = page->native_tab_id();
    if (tab < 0) return false;
    view->browser->extension_owner_tab_id = tab;
  }
  view->browser->is_extension_popup = true;
  g_weak_ref_set(&view->extension_owner, owner);
  return true;
}
void ephy_engine_views_preferences_changed(const char *property) {
  if (strcmp(property, "user-agent")) return;
  for (auto *view : views) view->apply_user_agent();
}
void ephy_engine_web_view_set_user_agent_override(EphyEngineView *view, const char *agent) {
  auto *d = data(view);
  d->has_user_agent_override = agent != nullptr;
  d->user_agent_override = agent ? agent : "";
  d->browser->set_user_agent_override(agent);
  d->apply_user_agent();
}
NO_STACK_PROTECTOR NOINLINE int ephy_engine_execute_process(int argc, char** argv) {
  if (!prevent_memory_core_dumps()) {
    g_printerr("Cannot disable Chromium memory core dumps; refusing to start.\n");
    return 1;
  }
  if (GnosisBrave::enabled()) {
    if (!GnosisBrave::composited()) g_setenv("GDK_BACKEND", "x11", true);
    return -1;
  }
  engine = new Engine;
  const int child = CefExecuteProcess(CefMainArgs(argc, argv), engine, nullptr);
  // Chromium resets child canaries on fork; do not unwind older embedder frames.
  if (child >= 0) std::exit(child);
  return child;
}
gboolean ephy_engine_initialize(int argc, char** argv, const char* profile, gboolean self_test) {
  try { runtime = new Application(profile); }
  catch (const std::exception& error) { g_printerr("Extensions: %s\n", error.what()); return false; }
  runtime->gtk_app = g_application_get_default();
  runtime->render_test = self_test;
  if (GnosisBrave::enabled()) {
    GnosisBrave::initialize(profile);
    ephy_engine_extensions_initialize_brave(&runtime->extensions);
    GnosisBrave::initialize_extensions([] {
      g_signal_emit_by_name(ephy_engine_session_get_default(), "extensions-changed");
    });
    return true;
  }
  engine->extension_paths = runtime->extensions.load_paths();
  for (const auto& ext : runtime->extensions.installed) if (ext.enabled) {
    runtime->extension_host->extension_ids.insert(ext.id);
    runtime->extension_host->permissions[ext.id] = ext.permissions;
    runtime->extension_host->extension_paths[ext.id] = ext.path;
  }
  runtime->extension_host->tab_bridge_id = Extensions::id_for_path(GNOSIS_BRIDGE_DIR);
  runtime->extension_host->tabs = [](CefRefPtr<CefBrowser> source) {
    std::vector<ExtensionTab> tabs;
    GtkRoot *current = nullptr;
    for (auto *view : views) if (source && view->browser->cef_browser() && view->browser->cef_browser()->IsSame(source)) {
      if (view->browser->is_extension_popup) {
        g_autoptr(EphyEngineView) owner = EPHY_ENGINE_VIEW(g_weak_ref_get(&view->extension_owner));
        if (!owner || data(owner)->disposed || data(owner)->host.private_mode) return tabs;
        current = gtk_widget_get_root(GTK_WIDGET(owner));
        if (!current) return tabs;
      } else current = gtk_widget_get_root(GTK_WIDGET(view->self));
      break;
    }
    // Background pages use the focused normal window; popup requests retain their owner's window.
    if (!current) for (auto *view : views) {
      if (view->disposed || view->host.private_mode || view->browser->is_extension_popup) continue;
      auto *root = gtk_widget_get_root(GTK_WIDGET(view->self));
      if (root && GTK_IS_WINDOW(root) && gtk_window_is_active(GTK_WINDOW(root))) { current = root; break; }
    }
    std::map<GtkRoot *, int> indices;
    static int next_window = 0;
    for (auto* view : views) {
      auto b = view->browser;
      auto *widget = GTK_WIDGET(view->self);
      auto *root = gtk_widget_get_root(widget);
      if (view->disposed || view->host.private_mode || b->is_extension_popup || !root ||
          !b->cef_browser() || b->native_tab_id < 0) continue;
      if (!current) current = root;
      int window = GPOINTER_TO_INT(g_object_get_data(G_OBJECT(root), "gnosis-extension-window-id"));
      if (!window) { window = ++next_window; g_object_set_data(G_OBJECT(root), "gnosis-extension-window-id", GINT_TO_POINTER(window)); }
      bool active = gtk_widget_get_mapped(widget);
      for (auto *parent = gtk_widget_get_parent(widget); parent; parent = gtk_widget_get_parent(parent)) {
        if (!ADW_IS_TAB_VIEW(parent)) continue;
        auto *page = adw_tab_view_get_selected_page(ADW_TAB_VIEW(parent));
        active = page && (widget == adw_tab_page_get_child(page) || gtk_widget_is_ancestor(widget, adw_tab_page_get_child(page)));
        break;
      }
      auto info = CefDictionaryValue::Create();
      info->SetInt("id", b->native_tab_id); info->SetInt("index", indices[root]++);
      info->SetInt("windowId", window); info->SetBool("currentWindow", root == current);
      info->SetString("url", b->url); info->SetString("title", b->name);
      info->SetString("status", b->loading ? "loading" : "complete");
      info->SetBool("active", active); info->SetBool("incognito", false);
      tabs.push_back({b->native_tab_id, b->cef_browser(), info});
    }
    return tabs;
  };
  runtime->extension_host->create = [](CefRefPtr<CefBrowser> source, const std::string& extension,
                                      const std::string& url, bool active, ExtensionHost::Created done) {
    if (!Policy::navigable(url) || (url.starts_with("chrome-extension://") &&
        !url.starts_with("chrome-extension://" + extension + "/"))) {
      done(nullptr, "The extension cannot open this URL"); return;
    }
    if (!source) {
      auto tabs = runtime->extension_host->tabs(nullptr);
      auto found = std::find_if(tabs.begin(), tabs.end(), [](const auto& tab) {
        return tab.info->GetBool("currentWindow") && tab.info->GetBool("active");
      });
      if (found != tabs.end()) source = found->browser;
    }
    if (!source) { done(nullptr, "The owning browser tab is unavailable"); return; }
    CefRefPtr<Browser> created;
    for (auto *view : views) if (view->browser->cef_browser() && view->browser->cef_browser()->IsSame(source) && !view->disposed && !view->host.private_mode) {
      created = view->host.open(url, active);
      break;
    }
    if (!created) { done(nullptr, "The owning browser tab is unavailable"); return; }
    struct Creating {
      CefRefPtr<Browser> browser;
      CefRefPtr<ExtensionHost> host;
      ExtensionHost::Created done;
      gint64 deadline;
    };
    g_timeout_add_full(G_PRIORITY_DEFAULT, 20, +[](gpointer pointer) -> gboolean {
      auto *pending = static_cast<Creating *>(pointer);
      if (pending->browser->closing || !runtime || g_get_monotonic_time() > pending->deadline) {
        pending->done(nullptr, "The new browser tab was closed or did not become ready");
        return G_SOURCE_REMOVE;
      }
      for (const auto& tab : pending->host->tabs(pending->browser->cef_browser())) {
        if (!pending->browser->cef_browser() || !tab.browser->IsSame(pending->browser->cef_browser())) continue;
        pending->done(tab.info->Copy(false), "");
        return G_SOURCE_REMOVE;
      }
      return G_SOURCE_CONTINUE;
    }, new Creating{created, runtime->extension_host, std::move(done), g_get_monotonic_time() + 10 * G_TIME_SPAN_SECOND},
    +[](gpointer pointer) { delete static_cast<Creating *>(pointer); });
  };
  runtime->extension_host->update = [](int id, const std::string& url, bool activate) {
    for (auto* view : views) if (view->browser->native_tab_id == id && !view->host.private_mode && !view->browser->is_extension_popup) {
      if (!url.empty()) ephy_engine_web_view_load_uri(view->self, url.c_str());
      if (activate) {
        auto* widget = GTK_WIDGET(view->self);
        for (auto* parent = gtk_widget_get_parent(widget); parent; parent = gtk_widget_get_parent(parent)) {
          if (!ADW_IS_TAB_VIEW(parent)) continue;
          for (int i=0;i<adw_tab_view_get_n_pages(ADW_TAB_VIEW(parent));i++) {
            auto* page = adw_tab_view_get_nth_page(ADW_TAB_VIEW(parent), i);
            if (gtk_widget_is_ancestor(widget, adw_tab_page_get_child(page))) adw_tab_view_set_selected_page(ADW_TAB_VIEW(parent), page);
          }
          break;
        }
      }
      break;
    }
  };
  CefSettings settings;
  settings.external_message_pump = true; settings.windowless_rendering_enabled = true;
  settings.no_sandbox = self_test;
  engine->diagnostic = self_test;
  settings.persist_session_cookies = true;
  std::string path = std::string(profile) + "/chromium";
  g_mkdir_with_parents(path.c_str(), 0700);
  CefString(&settings.root_cache_path) = path;
  CefString(&settings.cache_path) = path + "/profile";
  CefString(&settings.resources_dir_path) = CEF_RESOURCE_DIR;
  CefString(&settings.locales_dir_path) = std::string(CEF_RESOURCE_DIR) + "/locales";
  CefString(&settings.log_file) = std::string(profile) + "/chromium.log";
  settings.log_severity = LOGSEVERITY_WARNING;
  // Chromium 150's GTK4 DPI fallback calls the removed GTK3 GdkScreen API.
  // GTK scales logical 96-DPI fonts separately for each monitor.
  auto *gtk_settings = gtk_settings_get_default();
  int dpi = -1;
  g_object_get(gtk_settings, "gtk-xft-dpi", &dpi, nullptr);
  if (dpi <= 0) g_object_set(gtk_settings, "gtk-xft-dpi", 96 * 1024, nullptr);
  if (!CefInitialize(CefMainArgs(argc, argv), settings, engine, nullptr)) return false;
  pump = g_timeout_add(8, +[](gpointer) -> gboolean { CefDoMessageLoopWork(); return G_SOURCE_CONTINUE; }, nullptr);
  auto *catalog = ephy_engine_extensions_dup_installed();
  ephy_engine_extensions_initialize(catalog, CefRequestContext::GetGlobalContext(), [](JsonArray *snapshot) {
    if (!runtime) return;
    for (guint i = 0; i < json_array_get_length(snapshot); ++i) {
      auto *item = json_array_get_object_element(snapshot, i);
      runtime->extension_host->set_enabled(json_object_get_string_member(item, "id"),
        json_object_get_boolean_member_with_default(item, "available", false) &&
        json_object_get_boolean_member_with_default(item, "enabled", false));
    }
  }, &runtime->extensions);
  json_array_unref(catalog);
  return true;
}
void ephy_engine_set_automation_mode(gboolean enabled) { GnosisBrave::set_automation_mode(enabled); }
gboolean ephy_engine_prepare_shutdown() {
  if (!runtime) return true;
  if (GnosisBrave::enabled()) {
    for (auto* view : views) view->browser->shutdown();
    ephy_engine_extensions_close();
    GnosisBrave::shutdown();
    return true;
  }
  ephy_engine_downloads_shutdown();
  ephy_engine_extensions_close();
  std::vector<std::pair<CefRefPtr<Protocol>, CefRefPtr<Browser>>> closing;
  for (auto *view : views) closing.emplace_back(view->protocol, view->browser);
  for (auto& [protocol, browser] : closing) { protocol->stop(); browser->shutdown(); }
  auto deadline = g_get_monotonic_time() + 5 * G_TIME_SPAN_SECOND;
  do {
    CefDoMessageLoopWork();
    while (g_main_context_pending(nullptr)) g_main_context_iteration(nullptr, false);
    if (!Browser::active_count && ephy_engine_extensions_is_closed()) return true;
    g_usleep(1000);
  } while (g_get_monotonic_time() < deadline);
  return false;
}
void ephy_engine_shutdown() {
  if (GnosisBrave::enabled()) {
    ephy_engine_prepare_shutdown();
    delete runtime; runtime = nullptr;
    ephy_engine_clear_scheme_handlers();
    return;
  }
  if (!ephy_engine_prepare_shutdown()) {
    g_printerr("Chromium did not close its browsers; exiting without a core dump.\n");
    std::_Exit(1);
  }
  if (pump) { g_source_remove(pump); pump = 0; }
  ephy_engine_extensions_shutdown();
  if (runtime) { runtime->extension_host->stop(); delete runtime; runtime = nullptr; ephy_engine_sessions_shutdown(); CefShutdown(); }
  ephy_engine_clear_scheme_handlers();
  engine = nullptr;
}

EphyEngineHistoryItem* ephy_engine_back_forward_list_get_current_item(EphyEngineHistory* h) {
  return h->current >= 0 && h->current < int(h->items->size()) ? h->items->at(h->current) : nullptr;
}
EphyEngineHistoryItem* ephy_engine_back_forward_list_get_back_item(EphyEngineHistory* h) {
  return h->current > 0 ? h->items->at(h->current-1) : nullptr;
}
EphyEngineHistoryItem* ephy_engine_back_forward_list_get_forward_item(EphyEngineHistory* h) {
  return h->current+1 < int(h->items->size()) ? h->items->at(h->current+1) : nullptr;
}
GList* ephy_engine_back_forward_list_get_back_list_with_limit(EphyEngineHistory* h, guint limit) {
  GList* list = nullptr;
  for (int i=h->current-1; i>=0 && limit--; i--) list = g_list_append(list, h->items->at(i));
  return list;
}
GList* ephy_engine_back_forward_list_get_forward_list_with_limit(EphyEngineHistory* h, guint limit) {
  GList* list = nullptr;
  for (int i=h->current+1; i<int(h->items->size()) && limit--; i++) list = g_list_append(list, h->items->at(i));
  return list;
}
const gchar* ephy_engine_back_forward_list_item_get_uri(EphyEngineHistoryItem* i) { return i->uri; }
const gchar* ephy_engine_back_forward_list_item_get_original_uri(EphyEngineHistoryItem* i) { return i->uri; }
const gchar* ephy_engine_back_forward_list_item_get_title(EphyEngineHistoryItem* i) { return i->title; }
gboolean ephy_engine_web_view_is_controlled_by_automation(EphyEngineView*) { return false; }
void ephy_engine_web_view_try_close(EphyEngineView* v) { data(v)->browser->close(); }
namespace {
struct NativeCloseCall {
  GTask* task;
  std::weak_ptr<GnosisBrave::Page> page;
  gulong cancel_handler = 0;
  bool complete = false;
  ~NativeCloseCall() { g_object_unref(task); }
};
}
void ephy_engine_web_view_check_close(EphyEngineView* view, GCancellable* cancel,
                                     GAsyncReadyCallback callback, gpointer user) {
  g_return_if_fail(EPHY_IS_ENGINE_VIEW(view));
  auto call = std::make_shared<NativeCloseCall>();
  call->task = g_task_new(view, cancel, callback, user);
  g_task_set_source_tag(call->task, reinterpret_cast<gpointer>(ephy_engine_web_view_check_close));
  if (g_task_return_error_if_cancelled(call->task)) return;
  auto page = data(view)->browser->brave_page();
  if (!page) { g_task_return_int(call->task, 1); return; }
  call->page = page;
  if (cancel) call->cancel_handler = g_cancellable_connect(cancel, G_CALLBACK(+[](GCancellable*, gpointer data) {
    auto weak = *static_cast<std::weak_ptr<NativeCloseCall>*>(data);
    g_idle_add_full(G_PRIORITY_DEFAULT, +[](gpointer data) -> gboolean {
      if (auto call = static_cast<std::weak_ptr<NativeCloseCall>*>(data)->lock(); call && !call->complete)
        if (auto page = call->page.lock()) page->cancel_close();
      return G_SOURCE_REMOVE;
    }, new std::weak_ptr<NativeCloseCall>(weak), +[](gpointer data) { delete static_cast<std::weak_ptr<NativeCloseCall>*>(data); });
  }), new std::weak_ptr<NativeCloseCall>(call), +[](gpointer data) { delete static_cast<std::weak_ptr<NativeCloseCall>*>(data); });
  page->request_close([call](bool approved, bool prompted, const std::string& error) {
    call->complete = true;
    if (call->cancel_handler) {
      g_cancellable_disconnect(g_task_get_cancellable(call->task), call->cancel_handler);
      call->cancel_handler = 0;
    }
    if (g_task_return_error_if_cancelled(call->task)) return;
    if (!error.empty()) g_task_return_new_error(call->task, G_IO_ERROR, G_IO_ERROR_FAILED, "%s", error.c_str());
    else g_task_return_int(call->task, (approved ? 1 : 0) | (prompted ? 2 : 0));
  });
}
gboolean ephy_engine_web_view_check_close_finish(EphyEngineView* view, GAsyncResult* result,
                                                 gboolean* prompted, GError** error) {
  g_return_val_if_fail(g_task_is_valid(result, view), FALSE);
  const auto flags = g_task_propagate_int(G_TASK(result), error);
  if (prompted) *prompted = flags >= 0 && (flags & 2);
  return flags >= 0 && (flags & 1);
}
gboolean ephy_engine_web_view_close_is_approved(EphyEngineView* view) {
  g_return_val_if_fail(EPHY_IS_ENGINE_VIEW(view), FALSE);
  if (auto page = data(view)->browser->brave_page()) return page->close_is_approved();
  return TRUE;
}
gboolean ephy_engine_web_view_handle_vim_key(EphyEngineView* view, guint key, GdkModifierType modifiers) {
  if (auto page=data(view)->browser->brave_page()) return page->handle_vim_key(key,modifiers);
  return false;
}
void ephy_engine_web_view_cancel_close(EphyEngineView* view) {
  g_return_if_fail(EPHY_IS_ENGINE_VIEW(view));
  if (auto page = data(view)->browser->brave_page()) page->cancel_close();
}
void ephy_engine_web_view_load_uri(EphyEngineView* v, const gchar* uri) {
  auto *view = data(v);
  view->cancel_external_uri();
  view->stop_extension_context_probe();
  ++view->extension_context_generation;
  view->browser->navigate(uri ? uri : "about:blank");
}
void ephy_engine_web_view_register_page_message_handler(EphyEngineView* view, const char* name) {
  g_return_if_fail(EPHY_IS_ENGINE_VIEW(view));
  g_return_if_fail(name && *name && strlen(name) <= 128);
  data(view)->page_message_handlers.insert(name);
}
void ephy_engine_web_view_unregister_page_message_handler(EphyEngineView* view, const char* name) {
  g_return_if_fail(EPHY_IS_ENGINE_VIEW(view));
  g_return_if_fail(name);
  data(view)->page_message_handlers.erase(name);
}
gboolean ephy_engine_web_view_has_native_uri(EphyEngineView* view, const char* uri) {
  g_return_val_if_fail(EPHY_IS_ENGINE_VIEW(view), false);
  if (!uri) return false;
  auto *d = data(view);
  return !d->disposed && !d->detached && !d->browser->closing && d->browser->requested_uri() == uri;
}
gboolean ephy_engine_web_view_is_current_page(EphyEngineView* view, const char* uri) {
  if (!ephy_engine_web_view_has_native_uri(view, uri)) return false;
  if (auto page = data(view)->browser->brave_page()) return page->is_internal_page(uri);
  auto browser = data(view)->browser->cef_browser();
  auto frame = browser ? browser->GetMainFrame() : nullptr;
  return frame && frame->IsValid() && frame->GetURL().ToString() == uri;
}
gboolean ephy_engine_web_view_send_page_message(EphyEngineView* view, const char* expected_uri,
                                                const char* name, JsonNode* message) {
  g_return_val_if_fail(EPHY_IS_ENGINE_VIEW(view), false);
  if (!expected_uri || !name || !message || !ephy_engine_web_view_is_current_page(view, expected_uri)) return false;
  auto *d = data(view);
  const std::string uri = expected_uri;
  if (!uri.starts_with("ephy-about:") && !d->alternate_uris.contains(uri)) return false;
  g_autofree char *json = json_to_string(message, false);
  if (auto page = d->browser->brave_page()) return json && page->send_page_message(uri, name, json);
  return json && PageMessages::send(*d->browser, uri, name, json);
}
void ephy_engine_web_view_load_alternate_html(EphyEngineView* v, const gchar* content, const gchar* content_uri, const gchar*) {
  // CDP frameNavigated splits URL fragments into a separate field. Keep the
  // unique document token inside the opaque payload so every event has the
  // same lookup key and session saving never sees the data-URL transport.
  g_autofree char *id = g_uuid_string_random();
  const std::string document = "<!--gnosis-alternate-" + std::string(id) + "-->\n" + content;
  char* encoded = g_base64_encode(reinterpret_cast<const guchar*>(document.data()), document.size());
  std::string url = "data:text/html;charset=utf-8;base64," + std::string(encoded); g_free(encoded);
  auto *d = data(v);
  d->cancel_external_uri();
  // Keep the opaque data origin, but expose and save the intended page address.
  d->alternate_uris[url] = content_uri ? content_uri : "about:blank";
  d->browser->navigate(url);
  d->changed();
  if (!d->host.private_mode && content_uri && *content_uri) {
    struct CachedIcon { GWeakRef view; std::string uri; };
    auto* pending = new CachedIcon; g_weak_ref_init(&pending->view, v); pending->uri = content_uri;
    ephy_engine_favicon_database_get_favicon(ephy_engine_favicon_database_get_default(), content_uri, nullptr,
      +[](GObject* database, GAsyncResult* result, gpointer data) {
        std::unique_ptr<CachedIcon> pending(static_cast<CachedIcon*>(data));
        auto* view = EPHY_ENGINE_VIEW(g_weak_ref_get(&pending->view)); g_weak_ref_clear(&pending->view);
        g_autoptr(GError) error = nullptr;
        auto* icon = ephy_engine_favicon_database_get_favicon_finish(EPHY_ENGINE_FAVICON_DATABASE(database), result, &error);
        if (view) {
          auto* d = ViewData::data(view);
          if (!d->disposed && !d->host.private_mode && !d->favicon && d->visible_uri(d->browser->url) == pending->uri && icon) {
            g_set_object(&d->favicon, icon); g_object_notify(G_OBJECT(view), "favicon");
          }
          g_object_unref(view);
        }
        g_clear_object(&icon);
      }, pending);
  }
}
void ephy_engine_web_view_stop_loading(EphyEngineView* v) { if (auto page = data(v)->browser->brave_page()) { page->stop(); return; } if (auto b=data(v)->browser->cef_browser()) b->StopLoad(); }
gboolean ephy_engine_web_view_is_loading(EphyEngineView* v) { return data(v)->loading; }
gboolean ephy_engine_web_view_is_playing_audio(EphyEngineView* v) { auto b=data(v)->browser->cef_browser(); return b && b->GetHost()->IsAudioMuted()==false && false; }
void ephy_engine_web_view_set_is_muted(EphyEngineView* v, gboolean value) { data(v)->muted=value; if(auto b=data(v)->browser->cef_browser()) b->GetHost()->SetAudioMuted(value); g_object_notify(G_OBJECT(v), "is-muted"); }
gboolean ephy_engine_web_view_get_is_muted(EphyEngineView* v) { return data(v)->muted; }
guint64 ephy_engine_web_view_get_page_id(EphyEngineView* v) { return reinterpret_cast<guintptr>(v); }
const gchar* ephy_engine_web_view_get_title(EphyEngineView* v) { return data(v)->title.c_str(); }
void ephy_engine_web_view_reload(EphyEngineView* v) {
  auto *d = data(v);
  if (d->alternate_uris.contains(d->browser->url)) ephy_engine_web_view_load_uri(v, d->uri.c_str());
  else d->browser->reload();
}
void ephy_engine_web_view_reload_bypass_cache(EphyEngineView* v) {
  if (auto page = data(v)->browser->brave_page()) { page->reload(true); return; }
  if (data(v)->alternate_uris.contains(data(v)->browser->url)) ephy_engine_web_view_reload(v);
  else if(auto b=data(v)->browser->cef_browser()) b->ReloadIgnoreCache();
  else data(v)->browser->reload();
}
gdouble ephy_engine_web_view_get_estimated_load_progress(EphyEngineView* v) { return data(v)->progress; }
void ephy_engine_web_view_go_back(EphyEngineView* v) { data(v)->browser->back(); }
void ephy_engine_web_view_go_forward(EphyEngineView* v) { data(v)->browser->forward(); }
gboolean ephy_engine_web_view_can_go_back(EphyEngineView* v) { return data(v)->browser->can_back(); }
gboolean ephy_engine_web_view_can_go_forward(EphyEngineView* v) { return data(v)->browser->can_forward(); }
EphyEngineHistory* ephy_engine_web_view_get_back_forward_list(EphyEngineView* v) { return data(v)->history; }
void ephy_engine_web_view_go_to_back_forward_list_item(EphyEngineView* v, EphyEngineHistoryItem* item) {
  if (item->alternate || item->id < 0) { ephy_engine_web_view_load_uri(v, item->uri); return; }
  auto args=CefDictionaryValue::Create(); args->SetInt("entryId", item->id);
  data(v)->protocol->send("Page.navigateToHistoryEntry", args);
}
const gchar* ephy_engine_web_view_get_uri(EphyEngineView* v) { return data(v)->uri.empty() ? nullptr : data(v)->uri.c_str(); }
GdkTexture* ephy_engine_web_view_get_favicon(EphyEngineView* v) { return data(v)->favicon; }
const gchar* ephy_engine_web_view_get_custom_charset(EphyEngineView* v) { return data(v)->charset.empty()?nullptr:data(v)->charset.c_str(); }
void ephy_engine_web_view_set_custom_charset(EphyEngineView* v, const gchar* charset) {
  auto* d=data(v); d->charset=charset?charset:"";
  g_warning("Chromium does not expose a per-tab encoding override");
}
const EphyEnginePopupProperties* ephy_engine_web_view_get_popup_properties(EphyEngineView* v) { return &data(v)->popup_properties; }
namespace { std::string native_user_css, native_user_js; }
void ephy_engine_set_user_stylesheet(const char* source) {
  native_user_css = source ? source : "";
  GnosisBrave::set_user_content(native_user_css, native_user_js);
}
void ephy_engine_set_user_javascript(const char* source) {
  native_user_js = source ? source : "";
  GnosisBrave::set_user_content(native_user_css, native_user_js);
}
gboolean ephy_engine_web_view_show_password_manager(EphyEngineView* view) {
  if (auto page = data(view)->browser->brave_page()) { page->show_password_manager(); return true; }
  return false;
}
GtkWidget* ephy_engine_web_view_create_translation_controls(EphyEngineView* view) {
  if (auto page = data(view)->browser->brave_page()) return page->create_translation_controls();
  return nullptr;
}
void ephy_engine_web_view_set_zoom_level(EphyEngineView* v, gdouble zoom) {
  g_return_if_fail(std::isfinite(zoom));
  auto* d=data(v); d->zoom=std::clamp(zoom,.1,10.);
  if(auto page=d->browser->brave_page()) page->set_zoom(d->zoom);
  if(auto b=d->browser->cef_browser()) b->GetHost()->SetZoomLevel(std::log(d->zoom)/std::log(1.2));
  g_object_notify(G_OBJECT(v),"zoom-level");
}
gdouble ephy_engine_web_view_get_zoom_level(EphyEngineView* v) { return data(v)->zoom; }
void ephy_engine_web_view_execute_editing_command(EphyEngineView* v, const gchar* command) {
  auto* d=data(v);
  if (auto page = d->browser->brave_page()) { page->edit(command); return; }
  auto b=d->browser->cef_browser(); if(!b) return;
  auto frame=b->GetFocusedFrame(); if(!frame)frame=b->GetMainFrame();
  if(!g_ascii_strcasecmp(command,"Copy"))Clipboard::copy(*d->browser,false);
  else if(!g_ascii_strcasecmp(command,"Cut"))Clipboard::copy(*d->browser,true);
  else if(!g_ascii_strcasecmp(command,"Paste"))Clipboard::paste(*d->browser,false);
  else if(!g_ascii_strcasecmp(command,"PasteAsPlainText"))Clipboard::paste(*d->browser,true);
  else if(!g_ascii_strcasecmp(command,"SelectAll"))frame->SelectAll();
  else if(!g_ascii_strcasecmp(command,"Undo"))frame->Undo();
  else if(!g_ascii_strcasecmp(command,"Redo"))frame->Redo();
}
EphyEngineFind* ephy_engine_web_view_get_find_controller(EphyEngineView* v) { return data(v)->find; }
namespace {
struct NativeFindReply {
  GWeakRef controller, view;
  guint64 generation, document;
  guint limit;
  bool count_only, update_count;
  NativeFindReply(EphyEngineFind* find, guint maximum, bool count, bool update)
    : generation(find->generation), document(find->view->protocol->document_epoch),
      limit(maximum), count_only(count), update_count(update) {
    g_weak_ref_init(&controller, find); g_weak_ref_init(&view, find->view->self);
  }
  ~NativeFindReply() { g_weak_ref_clear(&controller); g_weak_ref_clear(&view); }
};
void native_find(EphyEngineFind* find, bool forward, bool count_only, bool update_count, guint limit) {
  auto page = find->view->browser->brave_page();
  if (!page) return;
  auto reply = std::make_shared<NativeFindReply>(find, limit, count_only, update_count);
  page->find(find->text ? find->text : "", forward,
    !(find->options & WEBKIT_FIND_OPTIONS_CASE_INSENSITIVE), !count_only,
    [reply](JsonObject* result, const std::string& error) {
      auto* view = EPHY_ENGINE_VIEW(g_weak_ref_get(&reply->view));
      auto* find = reinterpret_cast<EphyEngineFind*>(g_weak_ref_get(&reply->controller));
      if (!view || !find) { g_clear_object(&view); g_clear_object(&find); return; }
      auto* current = data(view);
      if (!current->disposed && !current->detached && current->find == find &&
          find->generation == reply->generation && current->protocol->document_epoch == reply->document) {
        const auto total = result && error.empty() ? json_object_get_int_member_with_default(result, "count", 0) : 0;
        const auto active = result && error.empty() ? json_object_get_int_member_with_default(result, "ordinal", 0) : 0;
        const auto count = static_cast<guint>(std::clamp<gint64>(total, 0, reply->limit ? reply->limit : G_MAXUINT));
        const auto ordinal = static_cast<guint>(std::clamp<gint64>(active, 0, count));
        if (reply->count_only || reply->update_count) g_signal_emit_by_name(find, "counted-matches", count);
        if (!reply->count_only) {
          g_signal_emit_by_name(find, "search-result", count, ordinal);
          if (count) g_signal_emit_by_name(find, "found-text", count);
          else g_signal_emit_by_name(find, "failed-to-find-text");
        }
      }
      g_object_unref(find); g_object_unref(view);
    });
}
}
void ephy_engine_find_controller_search(EphyEngineFind* f,const gchar* text,guint32 options,guint limit) {
  g_free(f->text); f->text=g_strdup(text); f->options=options;
  ++f->generation;
  if(f->view->browser->brave_page()) { native_find(f, !(options&WEBKIT_FIND_OPTIONS_BACKWARDS), false, true, limit); return; }
  if(auto b=f->view->browser->cef_browser())b->GetHost()->Find(text,!(options&WEBKIT_FIND_OPTIONS_BACKWARDS),!(options&WEBKIT_FIND_OPTIONS_CASE_INSENSITIVE),false);
}
void ephy_engine_find_controller_search_finish(EphyEngineFind* f) {
  ++f->generation;
  if(auto page=f->view->browser->brave_page()) { page->stop_find(); return; }
  if(auto b=f->view->browser->cef_browser())b->GetHost()->StopFinding(true);
}
void ephy_engine_find_controller_search_next(EphyEngineFind* f) {
  ++f->generation;
  if(f->view->browser->brave_page()) { native_find(f, true, false, false, G_MAXUINT); return; }
  if(auto b=f->view->browser->cef_browser())b->GetHost()->Find(f->text?f->text:"",true,!(f->options&WEBKIT_FIND_OPTIONS_CASE_INSENSITIVE),true);
}
void ephy_engine_find_controller_search_previous(EphyEngineFind* f) {
  ++f->generation;
  if(f->view->browser->brave_page()) { native_find(f, false, false, false, G_MAXUINT); return; }
  if(auto b=f->view->browser->cef_browser())b->GetHost()->Find(f->text?f->text:"",false,!(f->options&WEBKIT_FIND_OPTIONS_CASE_INSENSITIVE),true);
}
void ephy_engine_find_controller_count_matches(EphyEngineFind* f,const gchar* text,guint32 options,guint limit) {
  if(!f->view->browser->brave_page()) { ephy_engine_find_controller_search(f,text,options,limit); return; }
  g_free(f->text); f->text=g_strdup(text); f->options=options; ++f->generation;
  native_find(f, !(options&WEBKIT_FIND_OPTIONS_BACKWARDS), true, false, limit);
}
gboolean ephy_engine_find_controller_uses_native_results(EphyEngineFind* f) { return GnosisBrave::enabled() || bool(f->view->browser->brave_page()); }
gboolean ephy_engine_find_controller_supports_word_starts(EphyEngineFind* f) { return !ephy_engine_find_controller_uses_native_results(f); }
guint32 ephy_engine_find_controller_get_options(EphyEngineFind* f) { return f->options; }
gboolean ephy_engine_web_inspector_is_open(EphyEngineInspector* i) {
  if (auto page = i->view->browser->brave_page()) return page->devtools_open();
  auto browser = i->view->browser->cef_browser(); return browser && browser->GetHost()->HasDevTools();
}
void ephy_engine_web_inspector_show(EphyEngineInspector* i) {
  if (auto page = i->view->browser->brave_page()) { page->show_devtools(); return; }
  if(auto b=i->view->browser->cef_browser()) { CefWindowInfo info; CefBrowserSettings settings; b->GetHost()->ShowDevTools(info,nullptr,settings,CefPoint()); }
}
void ephy_engine_web_inspector_close(EphyEngineInspector* i) { if (auto page = i->view->browser->brave_page()) { page->close_devtools(); return; } if(auto b=i->view->browser->cef_browser())b->GetHost()->CloseDevTools(); }
namespace {
class ScriptEvaluation final : public std::enable_shared_from_this<ScriptEvaluation> {
public:
  ScriptEvaluation(EphyEngineView *view, const char *script, gssize length, const char *world,
                   const char *source, GCancellable *cancel, GAsyncReadyCallback callback, gpointer user)
    : task_(g_task_new(view, cancel, callback, user)), view_(data(view)), protocol_(view_->protocol),
      script_(script, length < 0 ? strlen(script) : size_t(length)), world_(world ? world : ""),
      epoch_(protocol_->document_epoch) {
    if (source && *source) {
      g_autofree char *uri = g_uri_escape_string(source, ":/?#[]@!$&'()*+,;=%", false);
      script_ += std::string("\n//# sourceURL=") + uri;
    }
  }
  ~ScriptEvaluation() { g_object_unref(task_); }
  void start() {
    auto self = shared_from_this();
    if (auto *cancel = g_task_get_cancellable(task_)) {
      cancel_handler_ = g_cancellable_connect(cancel, G_CALLBACK(+[](GCancellable*, gpointer p) {
        // Cancellation may originate off the GTK thread, or during connect().
        g_idle_add_full(G_PRIORITY_DEFAULT, +[](gpointer p) -> gboolean {
          if (auto self = static_cast<std::weak_ptr<ScriptEvaluation>*>(p)->lock()) {
            if (!self->complete_) {
              self->protocol_->fail(self->pending_, "Operation was cancelled");
              self->fail(G_IO_ERROR_CANCELLED, "Operation was cancelled");
            }
          }
          return G_SOURCE_REMOVE;
        }, new std::weak_ptr<ScriptEvaluation>(*static_cast<std::weak_ptr<ScriptEvaluation>*>(p)),
          +[](gpointer p) { delete static_cast<std::weak_ptr<ScriptEvaluation>*>(p); });
      }), new std::weak_ptr<ScriptEvaluation>(self),
        +[](gpointer p) { delete static_cast<std::weak_ptr<ScriptEvaluation>*>(p); });
    }
    if (!ready()) return;
    protocol_->evaluations[this] = self;
    pending_ = protocol_->send("Runtime.enable", nullptr, [self](auto, const auto& error) {
      if (self->ready(error)) self->get_frame();
    });
  }
  void context_changed() {
    if (!ready()) protocol_->pending.erase(pending_);
  }
private:
  GTask *task_;
  ViewData *view_;
  CefRefPtr<Protocol> protocol_;
  std::string script_, world_, context_;
  guint64 epoch_;
  gulong cancel_handler_ = 0;
  int pending_ = 0;
  bool complete_ = false;
  void finish() {
    complete_ = true;
    protocol_->evaluations.erase(this);
    if (cancel_handler_) {
      g_cancellable_disconnect(g_task_get_cancellable(task_), cancel_handler_);
      cancel_handler_ = 0;
    }
  }
  void fail(GIOErrorEnum code, const char *message) {
    if (complete_) return;
    finish();
    g_task_return_new_error(task_, G_IO_ERROR, code, "%s", message);
  }
  bool ready(const std::string& error = {}) {
    if (complete_) return false;
    auto *cancel = g_task_get_cancellable(task_);
    if (cancel && g_cancellable_is_cancelled(cancel)) fail(G_IO_ERROR_CANCELLED, "Operation was cancelled");
    else if (view_->disposed || view_->detached) fail(G_IO_ERROR_CLOSED, "Tab was closed or detached from its window");
    else if (protocol_->document_epoch != epoch_) fail(G_IO_ERROR_CANCELLED, "Document changed during script evaluation");
    else if (!context_.empty() && std::none_of(protocol_->contexts.begin(), protocol_->contexts.end(),
               [this](const auto& entry) { return entry.second.unique == context_; }))
      fail(G_IO_ERROR_CANCELLED, "The script execution context was destroyed");
    else if (!error.empty()) fail(G_IO_ERROR_FAILED, error.c_str());
    return !complete_;
  }
  void get_frame() {
    auto self = shared_from_this();
    pending_ = protocol_->send("Page.getFrameTree", nullptr, [self](auto reply, const auto& error) {
      if (!self->ready(error)) return;
      auto tree = reply ? reply->GetDictionary("frameTree") : nullptr;
      auto frame = tree ? tree->GetDictionary("frame") : nullptr;
      auto id = frame ? frame->GetString("id").ToString() : "";
      if (id.empty()) { self->fail(G_IO_ERROR_FAILED, "Chromium did not provide the document frame"); return; }
      self->protocol_->main_frame = id;
      if (self->world_.empty()) {
        for (const auto& [number, context] : self->protocol_->contexts) {
          if (context.main && context.frame == id) { self->evaluate(context.unique); return; }
        }
        self->fail(G_IO_ERROR_FAILED, "The document execution context is unavailable");
      } else self->create_world(id);
    });
  }
  void create_world(const std::string& frame) {
    auto args = CefDictionaryValue::Create();
    args->SetString("frameId", frame);
    args->SetString("worldName", "Gnosis page " + world_);
    args->SetBool("grantUniveralAccess", false);
    auto self = shared_from_this();
    pending_ = protocol_->send("Page.createIsolatedWorld", args, [self](auto reply, const auto& error) {
      if (!self->ready(error)) return;
      auto it = reply ? self->protocol_->contexts.find(reply->GetInt("executionContextId")) : self->protocol_->contexts.end();
      if (it == self->protocol_->contexts.end()) { self->fail(G_IO_ERROR_FAILED, "The isolated execution context is unavailable"); return; }
      self->evaluate(it->second.unique);
    });
  }
  void evaluate(const std::string& context) {
    if (context.empty()) { fail(G_IO_ERROR_FAILED, "Chromium did not provide a unique execution context"); return; }
    context_ = context;
    auto args = CefDictionaryValue::Create();
    args->SetString("expression", script_);
    // Numeric context IDs can be reused after navigation to another process.
    args->SetString("uniqueContextId", context);
    args->SetBool("returnByValue", true);
    args->SetBool("awaitPromise", true);
    auto self = shared_from_this();
    pending_ = protocol_->send("Runtime.evaluate", args, [self](auto reply, const auto& error) {
      if (!self->ready(error)) return;
      if (!reply || reply->HasKey("exceptionDetails")) {
        self->fail(G_IO_ERROR_FAILED, "JavaScript evaluation failed"); return;
      }
      auto remote = reply->GetDictionary("result");
      if (!remote || !remote->HasKey("type")) {
        self->fail(G_IO_ERROR_INVALID_DATA, "Chromium did not return a script result"); return;
      }
      if (remote->HasKey("objectId")) {
        auto release = CefDictionaryValue::Create();
        release->SetString("objectId", remote->GetString("objectId"));
        self->protocol_->send("Runtime.releaseObject", release);
        remote->Remove("objectId");
      }
      auto serialized = CefValue::Create(); serialized->SetDictionary(remote);
      auto json = CefWriteJSON(serialized, JSON_WRITER_DEFAULT).ToString();
      g_autoptr(JsonParser) parser = json_parser_new();
      GError *parse_error = nullptr;
      if (!json_parser_load_from_data(parser, json.c_str(), json.size(), &parse_error)) {
        self->fail(G_IO_ERROR_INVALID_DATA, parse_error->message);
        g_error_free(parse_error); return;
      }
      auto *value = json_object_ref(json_node_get_object(json_parser_get_root(parser)));
      self->finish();
      g_task_return_pointer(self->task_, value, reinterpret_cast<GDestroyNotify>(json_object_unref));
    });
  }
};
void Protocol::notify_evaluations() {
  // Chromium may destroy a document without replying to its awaited scripts.
  const auto watching = evaluations;
  for (const auto& [evaluation, weak] : watching)
    if (auto call = weak.lock()) call->context_changed();
}
}
void ephy_engine_web_view_evaluate_javascript(EphyEngineView *view, const char *script, gssize length,
                                            const char *world, const char *source, GCancellable *cancel,
                                            GAsyncReadyCallback callback, gpointer user) {
  g_return_if_fail(EPHY_IS_ENGINE_VIEW(view));
  g_return_if_fail(script);
  std::make_shared<ScriptEvaluation>(view, script, length, world, source, cancel, callback, user)->start();
}
JsonObject* ephy_engine_web_view_evaluate_javascript_finish(EphyEngineView *view, GAsyncResult *result, GError **error) {
  g_return_val_if_fail(g_task_is_valid(result, view), nullptr);
  return static_cast<JsonObject*>(g_task_propagate_pointer(G_TASK(result), error));
}
EphyEngineResource* ephy_engine_web_view_get_main_resource(EphyEngineView* v) { return data(v)->resource; }
EphyEngineInspector* ephy_engine_web_view_get_inspector(EphyEngineView* v) { return data(v)->inspector; }
const gchar* ephy_engine_web_resource_get_uri(EphyEngineResource* r) { return r->uri; }
EphyEngineResponse* ephy_engine_web_resource_get_response(EphyEngineResource* r) { return r->response; }
namespace {
class Source final : public CefStringVisitor {
public:
  explicit Source(GTask* t):task(t){}
  ~Source() override {
    if (task) {
      g_task_return_new_error(task, G_IO_ERROR, G_IO_ERROR_CLOSED, "The document was closed while reading its source");
      g_object_unref(task);
    }
  }
  void Visit(const CefString& string) override {
    auto text=string.ToString(); auto* bytes=g_bytes_new(text.data(),text.size());
    g_task_return_pointer(task,bytes,reinterpret_cast<GDestroyNotify>(g_bytes_unref));g_object_unref(task);task=nullptr;
  }
private:
  GTask* task;
  IMPLEMENT_REFCOUNTING(Source);
};
}
void ephy_engine_web_resource_get_data(EphyEngineResource* r,GCancellable* cancel,GAsyncReadyCallback cb,gpointer user) {
  auto* task=g_task_new(r,cancel,cb,user);
  if (g_task_return_error_if_cancelled(task)) { g_object_unref(task); return; }
  auto *view = EPHY_ENGINE_VIEW(g_weak_ref_get(&r->view));
  if (!view || data(view)->disposed || data(view)->resource != r) {
    g_task_return_new_error(task, G_IO_ERROR, G_IO_ERROR_CLOSED, "The resource's document is no longer active");
    g_object_unref(task);
  } else if (auto b=data(view)->browser->cef_browser()) b->GetMainFrame()->GetSource(new Source(task));
  else unsupported(task,"Browser is not ready");
  g_clear_object(&view);
}
guchar* ephy_engine_web_resource_get_data_finish(EphyEngineResource*,GAsyncResult* result,gsize* length,GError** error) {
  auto* bytes=static_cast<GBytes*>(g_task_propagate_pointer(G_TASK(result),error));if(!bytes)return nullptr;
  auto* copy=static_cast<guchar*>(g_bytes_unref_to_data(bytes,length));return copy;
}
void ephy_engine_web_view_save_to_file(EphyEngineView* v,GFile* file,WebKitSaveMode,GCancellable* cancel,GAsyncReadyCallback cb,gpointer user) {
  auto* task=g_task_new(v,cancel,cb,user); g_object_ref(file);
  auto args=CefDictionaryValue::Create(); args->SetString("format","mhtml");
  data(v)->protocol->send("Page.captureSnapshot",args,[task,file](auto reply,const auto& error) {
    if(!error.empty()||!reply)g_task_return_new_error(task,G_IO_ERROR,G_IO_ERROR_FAILED,"%s",error.c_str());
    else {
      auto text=reply->GetString("data").ToString(); GError* failure=nullptr;
      if(g_file_replace_contents(file,text.data(),text.size(),nullptr,false,G_FILE_CREATE_PRIVATE,nullptr,g_task_get_cancellable(task),&failure))g_task_return_boolean(task,true);
      else g_task_return_error(task,failure);
    }
    g_object_unref(file);g_object_unref(task);
  });
}
gboolean ephy_engine_web_view_get_tls_info(EphyEngineView* v,GTlsCertificate** certificate,GTlsCertificateFlags* errors) {
  if(certificate)*certificate=nullptr;if(errors)*errors=GTlsCertificateFlags(0);
  if (auto page = data(v)->browser->brave_page()) return page->tls_info(certificate, errors);
  auto b=data(v)->browser->cef_browser();if(!b)return false;
  auto entry=b->GetHost()->GetVisibleNavigationEntry();auto ssl=entry?entry->GetSSLStatus():nullptr;
  auto cert=ssl?ssl->GetX509Certificate():nullptr;if(!cert)return false;
  if(errors)*errors=ephy_engine_certificate_errors(ssl->GetCertStatus(),cert);
  if(certificate) {
    *certificate=ephy_engine_certificate_to_tls(cert);
    if(!*certificate)return false;
  }
  return true;
}
gboolean ephy_engine_web_view_tls_is_secure(EphyEngineView* view) {
  if (auto page = data(view)->browser->brave_page()) return page->tls_secure();
  return true;
}
void ephy_engine_web_view_get_snapshot(EphyEngineView* v,WebKitSnapshotRegion region,WebKitSnapshotOptions,GCancellable* cancel,GAsyncReadyCallback cb,gpointer user) {
  auto* task=g_task_new(v,cancel,cb,user);
  if (g_task_return_error_if_cancelled(task)) { g_object_unref(task); return; }
  if (auto page = data(v)->browser->brave_page()) {
    page->snapshot([task](GdkTexture* texture, const std::string& error) {
      if (g_task_return_error_if_cancelled(task)) { g_object_unref(task); return; }
      if (texture) g_task_return_pointer(task, g_object_ref(texture), g_object_unref);
      else g_task_return_new_error(task, G_IO_ERROR, G_IO_ERROR_FAILED, "%s", error.c_str());
      g_object_unref(task);
    }, region == WEBKIT_SNAPSHOT_REGION_FULL_DOCUMENT);
    return;
  }
  auto* texture=data(v)->browser->snapshot_texture();
  if(!texture){unsupported(task,"The tab has no readable CEF frame");return;}
  g_task_return_pointer(task,texture,g_object_unref);g_object_unref(task);
}
GdkTexture* ephy_engine_web_view_get_snapshot_finish(EphyEngineView*,GAsyncResult* r,GError** e) { return GDK_TEXTURE(g_task_propagate_pointer(G_TASK(r),e)); }
WebKitUserContentManager* ephy_engine_web_view_get_user_content_manager(EphyEngineView* v) { return data(v)->ucm; }
EphyEngineSessionState* ephy_engine_web_view_get_session_state(EphyEngineView* v) {
  auto* d=data(v);auto* value=g_variant_ref_sink(g_variant_new("(ss)",d->uri.c_str(),d->title.c_str()));auto* bytes=g_variant_get_data_as_bytes(value);
  auto* state=ephy_engine_web_view_session_state_new(bytes);g_bytes_unref(bytes);g_variant_unref(value);return state;
}
void ephy_engine_web_view_restore_session_state(EphyEngineView* v,EphyEngineSessionState* state) {
  auto* value=g_variant_ref_sink(g_variant_new_from_bytes(G_VARIANT_TYPE("(ss)"),state->bytes,false));
  if(g_variant_is_normal_form(value)) {
    const char *uri,*title; g_variant_get(value,"(&s&s)",&uri,&title);
    auto *d = data(v);
    for (auto *item : *d->history->items) g_object_unref(item);
    d->history->items->clear();
    auto *item = reinterpret_cast<EphyEngineHistoryItem*>(g_object_new(ephy_engine_history_item_get_type(), nullptr));
    item->uri = g_strdup(uri); item->title = g_strdup(title); item->id = -1;
    d->history->items->push_back(item); d->history->current = 0;
    d->browser->url=uri; d->browser->name=title; d->changed();
    g_signal_emit_by_name(d->history, "changed", nullptr, nullptr);
  }
  g_variant_unref(value);
}
EphyEngineSessionState* ephy_engine_web_view_session_state_new(GBytes* bytes) { auto* state=new EphyEngineSessionState;g_atomic_ref_count_init(&state->refs);state->bytes=g_bytes_ref(bytes);return state; }
EphyEngineSessionState* ephy_engine_web_view_session_state_ref(EphyEngineSessionState* s) { g_atomic_ref_count_inc(&s->refs);return s; }
void ephy_engine_web_view_session_state_unref(EphyEngineSessionState* s) { if(g_atomic_ref_count_dec(&s->refs)){g_bytes_unref(s->bytes);delete s;} }
GBytes* ephy_engine_web_view_session_state_serialize(EphyEngineSessionState* s) { return g_bytes_ref(s->bytes); }
void ephy_engine_web_view_send_message_to_page(EphyEngineView* v,WebKitUserMessage*,GCancellable* cancel,GAsyncReadyCallback cb,gpointer user) { unsupported(g_task_new(v,cancel,cb,user),"WebKit web-process messages are unavailable on Chromium"); }
WebKitUserMessage* ephy_engine_web_view_send_message_to_page_finish(EphyEngineView*,GAsyncResult* r,GError** e) { return WEBKIT_USER_MESSAGE(g_task_propagate_pointer(G_TASK(r),e)); }
void ephy_engine_web_view_set_cors_allowlist(EphyEngineView*,const gchar* const*) { /* Internal schemes enforce their own access policy. */ }
gboolean ephy_engine_web_view_get_is_web_process_responsive(EphyEngineView* v) { return !data(v)->disposed; }
void ephy_engine_web_view_terminate_web_process(EphyEngineView* v) { data(v)->protocol->send("Page.crash",nullptr); }

EphyEngineView* ephy_engine_view_for_browser(int id) {
  for(auto* view:views) if(auto b=view->browser->cef_browser();b&&b->GetIdentifier()==id&&!view->disposed)return view->self;
  return nullptr;
}

CefRefPtr<CefBrowser> ephy_engine_view_browser(EphyEngineView* view) {return data(view)->browser->cef_browser();}

std::shared_ptr<GnosisBrave::Page> ephy_engine_view_brave_page(EphyEngineView* view) { return data(view)->browser->brave_page(); }
guint64 ephy_engine_web_view_get_document_epoch(EphyEngineView* view) { return data(view)->protocol->document_epoch; }
