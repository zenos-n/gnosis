/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "config.h"
#include "gnosis-mcp.h"
#include "ephy-window.h"
#include "ephy-embed-shell.h"
#include "ephy-embed-container.h"
#include "brave/browser.hpp"
#include <algorithm>
#include <cmath>
#include <functional>
#include <memory>
#include <map>
#include <stdexcept>
#include <string>
#include <vector>

std::shared_ptr<GnosisBrave::Page> ephy_engine_view_brave_page(EphyEngineView*);

namespace {
constexpr const char* path = "/com/neg_zero/Gnosis/BrowserControl";
constexpr const char* ai_window_key = "gnosis-ai-window";
struct Request;
GApplication* application = nullptr;
GDBusConnection* connection = nullptr;
guint registration = 0, owner_watch = 0;
guint64 generation = 0;
guint64 selected_window = 0;
std::string mode = "off", controller;
std::weak_ptr<Request> pending;
EphyWindow* ai_window = nullptr;

std::string encode(JsonObject* object) {
  g_autoptr(JsonNode) node = json_node_new(JSON_NODE_OBJECT);
  json_node_set_object(node, object);
  g_autofree char* json = json_to_string(node, FALSE);
  return json;
}
std::string string(JsonObject* object, const char* name, bool required = false, size_t limit = 65536) {
  auto* node = json_object_get_member(object, name);
  if (!node) {
    if (required) throw std::runtime_error(std::string("Missing argument: ") + name);
    return {};
  }
  if (!JSON_NODE_HOLDS_VALUE(node) || json_node_get_value_type(node) != G_TYPE_STRING)
    throw std::runtime_error(std::string("Expected a string: ") + name);
  std::string result = json_node_get_string(node);
  if (result.size() > limit) throw std::runtime_error(std::string("Argument is too long: ") + name);
  return result;
}
double number(JsonObject* object, const char* name, double fallback, double limit = 100000) {
  auto* node = json_object_get_member(object, name);
  if (!node) return fallback;
  auto type = json_node_get_value_type(node);
  if (!JSON_NODE_HOLDS_VALUE(node) || (type != G_TYPE_DOUBLE && type != G_TYPE_INT64))
    throw std::runtime_error(std::string("Expected a number: ") + name);
  double value = json_node_get_double(node);
  if (!std::isfinite(value) || std::abs(value) > limit)
    throw std::runtime_error(std::string("Number is outside the allowed range: ") + name);
  return value;
}
bool boolean(JsonObject* object, const char* name, bool fallback) {
  auto* node = json_object_get_member(object, name);
  if (!node) return fallback;
  if (!JSON_NODE_HOLDS_VALUE(node) || json_node_get_value_type(node) != G_TYPE_BOOLEAN)
    throw std::runtime_error(std::string("Expected a boolean: ") + name);
  return json_node_get_boolean(node);
}
void reply(GDBusMethodInvocation* invocation, const std::string& text, bool error, GBytes* png = nullptr) {
  g_autoptr(JsonObject) result = json_object_new();
  auto* content = json_array_new();
  auto* item = json_object_new();
  json_object_set_string_member(item, "type", "text");
  json_object_set_string_member(item, "text", text.c_str());
  json_array_add_object_element(content, item);
  if (png) {
    gsize size = 0;
    auto* bytes = static_cast<const guchar*>(g_bytes_get_data(png, &size));
    g_autofree char* encoded = g_base64_encode(bytes, size);
    item = json_object_new();
    json_object_set_string_member(item, "type", "image");
    json_object_set_string_member(item, "mimeType", "image/png");
    json_object_set_string_member(item, "data", encoded);
    json_array_add_object_element(content, item);
  }
  json_object_set_array_member(result, "content", content);
  json_object_set_boolean_member(result, "isError", error);
  g_dbus_method_invocation_return_value(invocation, g_variant_new("(s)", encode(result).c_str()));
}
bool regular(EphyWebView* view) {
  EphyEngineSession* session = nullptr;
  g_object_get(view, "network-session", &session, nullptr);
  bool result = session && !ephy_engine_session_is_ephemeral(session);
  g_clear_object(&session);
  return result;
}
bool eligible(EphyWindow* window) {
  bool owned = g_object_get_data(G_OBJECT(window), ai_window_key) != nullptr;
  return mode == "shared" ? !owned : mode == "separate" && owned;
}
struct Tab { EphyWindow* window; EphyEmbed* embed; EphyWebView* view; };
std::vector<Tab> tabs() {
  std::vector<Tab> result;
  if (!application) return result;
  for (auto* it = gtk_application_get_windows(GTK_APPLICATION(application)); it; it = it->next) {
    if (!EPHY_IS_WINDOW(it->data)) continue;
    auto* window = EPHY_WINDOW(it->data);
    if (!eligible(window)) continue;
    auto* tab_view = ephy_window_get_tab_view(window);
    for (int i = 0; i < ephy_tab_view_get_n_pages(tab_view); ++i) {
      auto* embed = EPHY_EMBED(ephy_tab_view_get_nth_page(tab_view, i));
      auto* view = ephy_embed_get_web_view(embed);
      if (regular(view)) result.push_back({window, embed, view});
    }
  }
  return result;
}
EphyWindow* find_window(JsonObject* args) {
  auto id = string(args, "window_id", false, 20);
  if (id.empty() && selected_window) id = std::to_string(selected_window);
  for (auto tab : tabs()) {
    if (id.empty() || id == std::to_string(ephy_window_get_uid(tab.window))) return tab.window;
  }
  throw std::runtime_error(id.empty() ? "No window is available in this browsing mode." :
    "Window is closed, private, or outside the selected browsing mode. Use list_windows and activate_window to choose another.");
}
Tab find_tab(JsonObject* args, bool select) {
  auto id = string(args, "tab_id", false, 20);
  auto* window = id.empty() ? find_window(args) : nullptr;
  auto available = tabs();
  for (auto tab : available) {
    if ((!id.empty() && id == std::to_string(ephy_web_view_get_uid(tab.view))) ||
        (id.empty() && tab.window == window && ephy_window_get_active_embed(tab.window) == tab.embed)) {
      if (select) {
        ephy_tab_view_select_page(ephy_window_get_tab_view(tab.window), GTK_WIDGET(tab.embed));
        selected_window = ephy_window_get_uid(tab.window);
      }
      return tab;
    }
  }
  throw std::runtime_error(id.empty() ? "No tab is available in this browsing mode." : "Tab is closed, private, or outside the selected browsing mode.");
}
JsonObject* tab_info(const Tab& tab) {
  auto* result = json_object_new();
  json_object_set_string_member(result, "tab_id", std::to_string(ephy_web_view_get_uid(tab.view)).c_str());
  json_object_set_string_member(result, "window_id", std::to_string(ephy_window_get_uid(tab.window)).c_str());
  const char* uri = ephy_engine_web_view_get_uri(EPHY_ENGINE_VIEW(tab.view));
  const char* title = ephy_engine_web_view_get_title(EPHY_ENGINE_VIEW(tab.view));
  json_object_set_string_member(result, "url", uri ? uri : "");
  json_object_set_string_member(result, "title", title ? title : "");
  json_object_set_boolean_member(result, "active", ephy_window_get_active_embed(tab.window) == tab.embed);
  json_object_set_boolean_member(result, "loading", ephy_engine_web_view_is_loading(EPHY_ENGINE_VIEW(tab.view)));
  return result;
}
JsonObject* window_info(EphyWindow* window) {
  auto* result = json_object_new();
  auto id = ephy_window_get_uid(window);
  json_object_set_string_member(result, "window_id", std::to_string(id).c_str());
  const char* title = gtk_window_get_title(GTK_WINDOW(window));
  json_object_set_string_member(result, "title", title ? title : "");
  json_object_set_boolean_member(result, "active", gtk_window_is_active(GTK_WINDOW(window)));
  json_object_set_boolean_member(result, "selected", selected_window == id);
  auto* array = json_array_new();
  for (auto tab : tabs()) if (tab.window == window) json_array_add_object_element(array, tab_info(tab));
  json_object_set_array_member(result, "tabs", array);
  return result;
}
EphyWindow* separate_window() {
  if (!ai_window) {
    ai_window = ephy_window_new();
    g_object_add_weak_pointer(G_OBJECT(ai_window), reinterpret_cast<gpointer*>(&ai_window));
    g_object_set_data(G_OBJECT(ai_window), ai_window_key, GINT_TO_POINTER(1));
  }
  return ai_window;
}
void hide_cursors() {
  if (!application) return;
  for (auto* it = gtk_application_get_windows(GTK_APPLICATION(application)); it; it = it->next)
    if (EPHY_IS_WINDOW(it->data)) ephy_window_set_ai_cursor(EPHY_WINDOW(it->data), nullptr, 0, 0, 0, 0);
}
bool web_url(const std::string& url) {
  if (url == "about:blank") return true;
  g_autoptr(GUri) uri = g_uri_parse(url.c_str(), G_URI_FLAGS_NONE, nullptr);
  return uri && g_uri_get_host(uri) && !g_uri_get_userinfo(uri) &&
    (g_strcmp0(g_uri_get_scheme(uri), "http") == 0 || g_strcmp0(g_uri_get_scheme(uri), "https") == 0);
}

struct Request : std::enable_shared_from_this<Request> {
  GDBusMethodInvocation* invocation;
  EphyWebView* view = nullptr;
  GCancellable* cancel = g_cancellable_new();
  guint64 epoch = generation;
  guint64 document_epoch = 0;
  bool preserve_document = false;
  guint timeout = 0;
  gulong navigation_signal = 0;
  bool navigation_finished = false;
  guint64 navigation_epoch = 0;
  std::string target_uri;
  bool done = false, pressed = false;
  double x = 0, y = 0;
  explicit Request(GDBusMethodInvocation* call) : invocation(G_DBUS_METHOD_INVOCATION(g_object_ref(call))) {}
  ~Request() {
    if (view && navigation_signal) g_signal_handler_disconnect(view, navigation_signal);
    g_clear_object(&view); g_clear_object(&cancel); g_object_unref(invocation);
    if (timeout) g_source_remove(timeout);
  }
  void start() {
    auto* weak = new std::weak_ptr<Request>(shared_from_this());
    timeout = g_timeout_add_seconds_full(G_PRIORITY_DEFAULT, 25, +[](gpointer p) -> gboolean {
      if (auto self = static_cast<std::weak_ptr<Request>*>(p)->lock()) {
        self->timeout = 0; self->fail("The browser operation timed out.");
      }
      return G_SOURCE_REMOVE;
    }, weak, +[](gpointer p) { delete static_cast<std::weak_ptr<Request>*>(p); });
  }
  bool valid() {
    if (done) return false;
    if (!application || mode == "off" || epoch != generation ||
        controller != g_dbus_method_invocation_get_sender(invocation)) {
      fail("AI browsing stopped or its mode changed."); return false;
    }
    if (view) {
      if (preserve_document && document_epoch != ephy_engine_web_view_get_document_epoch(EPHY_ENGINE_VIEW(view))) {
        fail("The document changed. Take a new snapshot before interacting."); return false;
      }
      auto available = tabs();
      auto it = std::find_if(available.begin(), available.end(), [this](const Tab& tab) { return tab.view == view; });
      if (it == available.end() || ephy_window_get_active_embed(it->window) != it->embed) {
        fail("The controlled tab was closed, moved, or deselected."); return false;
      }
    }
    return true;
  }
  void finish(const std::string& text, bool error = false, GBytes* png = nullptr) {
    if (done) return;
    done = true;
    if (view && navigation_signal) { g_signal_handler_disconnect(view, navigation_signal); navigation_signal = 0; }
    if (timeout) { g_source_remove(timeout); timeout = 0; }
    g_cancellable_cancel(cancel);
    if (pressed && view) {
      if (auto page = ephy_engine_view_brave_page(EPHY_ENGINE_VIEW(view)))
        page->command("Input.dispatchMouseEvent", "{\"type\":\"mouseReleased\",\"button\":\"left\",\"clickCount\":1,\"x\":" + std::to_string(x) + ",\"y\":" + std::to_string(y) + "}");
      pressed = false;
    }
    reply(invocation, text, error, png);
  }
  void fail(const std::string& text) { finish(text, true); }
  void success(JsonObject* result) { finish(encode(result)); }
  void success() { finish("{\"ok\":true}"); }
  void command(const char* method, JsonObject* params, std::function<void(JsonObject*)> callback) {
    if (!valid()) return;
    auto page = ephy_engine_view_brave_page(EPHY_ENGINE_VIEW(view));
    if (!page || page->is_closed() || page->session.empty()) { fail("The browser tab is not ready."); return; }
    page->command(method, encode(params), [self = shared_from_this(), callback](JsonObject* result, const std::string& error) {
      if (!self->valid()) return;
      if (!error.empty()) self->fail(error);
      else callback(result);
    });
  }
  void script(JsonObject* args, std::function<void(JsonObject*)> callback) {
    if (!valid()) return;
    g_autoptr(GError) error = nullptr;
    g_autoptr(GBytes) resource = g_resources_lookup_data("/com/neg_zero/Gnosis/mcp/page.js", G_RESOURCE_LOOKUP_FLAGS_NONE, &error);
    if (!resource) { fail(error->message); return; }
    gsize length = 0;
    auto* data = static_cast<const char*>(g_bytes_get_data(resource, &length));
    std::string code = "(" + std::string(data, length) + ")(" + encode(args) + ")";
    struct Evaluation { std::shared_ptr<Request> self; std::function<void(JsonObject*)> callback; };
    ephy_engine_web_view_evaluate_javascript(EPHY_ENGINE_VIEW(view), code.c_str(), code.size(), "gnosis-mcp",
      nullptr, cancel, +[](GObject* source, GAsyncResult* result, gpointer data) {
        std::unique_ptr<Evaluation> evaluation(static_cast<Evaluation*>(data));
        g_autoptr(GError) error = nullptr;
        g_autoptr(JsonObject) value = ephy_engine_web_view_evaluate_javascript_finish(EPHY_ENGINE_VIEW(source), result, &error);
        auto self = evaluation->self;
        if (!self->valid()) return;
        if (error) { self->fail(error->message); return; }
        auto* node = value ? json_object_get_member(value, "value") : nullptr;
        if (!node || !JSON_NODE_HOLDS_OBJECT(node)) { self->fail("The document returned no result."); return; }
        evaluation->callback(json_node_get_object(node));
      }, new Evaluation{shared_from_this(), std::move(callback)});
  }
  void wait_loaded(const std::string& before, gint64 started) {
    struct Wait { std::shared_ptr<Request> self; std::string before; gint64 started; };
    g_timeout_add_full(G_PRIORITY_DEFAULT, 50, +[](gpointer p) -> gboolean {
      auto* wait = static_cast<Wait*>(p); wait->self->loaded(wait->before, wait->started); return G_SOURCE_REMOVE;
    }, new Wait{shared_from_this(), before, started}, +[](gpointer p) { delete static_cast<Wait*>(p); });
  }
  void loaded(const std::string& before, gint64 started) {
    if (!valid()) return;
    if (!navigation_signal) {
      navigation_epoch = ephy_engine_web_view_get_document_epoch(EPHY_ENGINE_VIEW(view));
      navigation_signal = g_signal_connect(view, "load-changed", G_CALLBACK(+[](EphyWebView*, WebKitLoadEvent event, gpointer data) {
        if (event == WEBKIT_LOAD_FINISHED) static_cast<Request*>(data)->navigation_finished = true;
      }), this);
    }
    auto page = ephy_engine_view_brave_page(EPHY_ENGINE_VIEW(view));
    if (!page || page->session.empty() || ephy_engine_web_view_is_loading(EPHY_ENGINE_VIEW(view)) ||
        g_get_monotonic_time() - started < 300000) { wait_loaded(before, started); return; }
    // The shell can already show the requested URL while Brave is still on its bootstrap document.
    page->command("Runtime.evaluate", "{\"expression\":\"({url:location.href,ready:document.readyState})\",\"returnByValue\":true}",
      [self = shared_from_this(), before, started](JsonObject* result, const std::string& error) {
        if (!self->valid()) return;
        auto* remote = result && json_object_has_member(result, "result") ? json_object_get_object_member(result, "result") : nullptr;
        auto* value = remote ? json_object_get_member(remote, "value") : nullptr;
        if (error.empty() && value && JSON_NODE_HOLDS_OBJECT(value)) {
          auto* document = json_node_get_object(value);
          const char* uri = json_object_get_string_member_with_default(document, "url", "");
          const char* ready = json_object_get_string_member_with_default(document, "ready", "");
          bool arrived = self->target_uri.empty() ? self->navigation_finished || before != uri :
            self->target_uri == uri || self->navigation_finished;
          bool reloaded = before.empty() || before != self->target_uri || self->navigation_finished ||
            self->navigation_epoch != ephy_engine_web_view_get_document_epoch(EPHY_ENGINE_VIEW(self->view));
          if (arrived && reloaded && g_str_equal(ready, "complete") &&
              !ephy_engine_web_view_is_loading(EPHY_ENGINE_VIEW(self->view))) {
            for (auto tab : tabs()) if (tab.view == self->view) {
              g_autoptr(JsonObject) info = tab_info(tab); self->success(info); return;
            }
          }
        }
        self->wait_loaded(before, started);
      });
  }
  void point(JsonObject* args, std::function<void()> callback) {
    g_autoptr(JsonObject) params = json_object_new();
    json_object_set_string_member(params, "operation", "point");
    auto ref = string(args, "ref", false, 128);
    if (!ref.empty()) json_object_set_string_member(params, "ref", ref.c_str());
    else {
      if (!json_object_has_member(args, "x") || !json_object_has_member(args, "y"))
        throw std::runtime_error("Provide an element reference or both x and y.");
      json_object_set_double_member(params, "x", number(args, "x", 0));
      json_object_set_double_member(params, "y", number(args, "y", 0));
    }
    script(params, [self = shared_from_this(), callback](JsonObject* result) {
      self->x = json_object_get_double_member(result, "x");
      self->y = json_object_get_double_member(result, "y");
      auto* viewport = json_object_get_object_member(result, "viewport");
      for (auto tab : tabs()) if (tab.view == self->view)
        ephy_window_set_ai_cursor(tab.window, tab.view, self->x, self->y,
          json_object_get_double_member(viewport, "width"), json_object_get_double_member(viewport, "height"));
      callback();
    });
  }
  void mouse(const char* type, std::function<void()> callback) {
    g_autoptr(JsonObject) params = json_object_new();
    json_object_set_string_member(params, "type", type);
    json_object_set_double_member(params, "x", x);
    json_object_set_double_member(params, "y", y);
    if (g_strcmp0(type, "mouseMoved") != 0) {
      json_object_set_string_member(params, "button", "left");
      json_object_set_int_member(params, "clickCount", 1);
      json_object_set_int_member(params, "buttons", g_strcmp0(type, "mousePressed") == 0 ? 1 : 0);
      if (g_strcmp0(type, "mousePressed") == 0) pressed = true;
    }
    bool releasing = g_strcmp0(type, "mouseReleased") == 0;
    command("Input.dispatchMouseEvent", params, [self = shared_from_this(), releasing, callback](JsonObject*) {
      if (releasing) self->pressed = false;
      callback();
    });
  }
  void click(std::function<void()> callback) {
    mouse("mouseMoved", [self = shared_from_this(), callback] {
      self->mouse("mousePressed", [self, callback] { self->mouse("mouseReleased", callback); });
    });
  }
  void key(const std::string& key, std::function<void()> callback) {
    int modifiers = 0;
    std::string name = key;
    for (size_t pos; (pos = name.find('+')) != std::string::npos;) {
      auto modifier = name.substr(0, pos); name.erase(0, pos + 1);
      if (modifier == "Control" || modifier == "Ctrl") modifiers |= 2;
      else if (modifier == "Shift") modifiers |= 8;
      else if (modifier == "Alt") modifiers |= 1;
      else if (modifier == "Meta") modifiers |= 4;
      else throw std::runtime_error("Unknown key modifier.");
    }
    static const std::map<std::string, std::pair<int, const char*>> keys = {
      {"Enter",{13,"Enter"}}, {"Tab",{9,"Tab"}}, {"Escape",{27,"Escape"}}, {"Backspace",{8,"Backspace"}},
      {"Delete",{46,"Delete"}}, {"ArrowLeft",{37,"ArrowLeft"}}, {"ArrowUp",{38,"ArrowUp"}},
      {"ArrowRight",{39,"ArrowRight"}}, {"ArrowDown",{40,"ArrowDown"}}, {"Home",{36,"Home"}},
      {"End",{35,"End"}}, {"PageUp",{33,"PageUp"}}, {"PageDown",{34,"PageDown"}}, {"Space",{32,"Space"}}
    };
    int code = 0; std::string physical, text;
    if (auto it = keys.find(name); it != keys.end()) { code = it->second.first; physical = it->second.second; }
    else if (name.size() == 1 && g_ascii_isalnum(name[0])) {
      code = g_ascii_toupper(name[0]); physical = std::string(g_ascii_isdigit(name[0]) ? "Digit" : "Key") + char(code);
      if (!(modifiers & (1|2|4))) text = name;
    } else throw std::runtime_error("Unsupported key. Use a named key or ASCII letter/digit.");
    if (name == "Enter" && !(modifiers & (1|2|4))) text = "\r";
    if (name == "Space" && !(modifiers & (1|2|4))) text = " ";
    g_autoptr(JsonObject) params = json_object_new();
    json_object_set_string_member(params, "type", text.empty() ? "rawKeyDown" : "keyDown");
    json_object_set_string_member(params, "key", name == "Space" ? " " : name.c_str());
    json_object_set_string_member(params, "code", physical.c_str());
    json_object_set_int_member(params, "windowsVirtualKeyCode", code);
    json_object_set_int_member(params, "modifiers", modifiers);
    if (!text.empty()) json_object_set_string_member(params, "text", text.c_str());
    auto up = std::shared_ptr<JsonObject>(json_object_ref(params), json_object_unref);
    command("Input.dispatchKeyEvent", params, [self = shared_from_this(), up, callback](JsonObject*) {
      json_object_set_string_member(up.get(), "type", "keyUp"); json_object_remove_member(up.get(), "text");
      self->command("Input.dispatchKeyEvent", up.get(), [callback](JsonObject*) { callback(); });
    });
  }
};

void set_mode(GSimpleAction* action, GVariant* value, gpointer) {
  std::string next = g_variant_get_string(value, nullptr);
  if (next != "off" && next != "shared" && next != "separate") return;
  if (mode != next) {
    ++generation;
    if (auto request = pending.lock()) request->fail("AI browsing stopped or its mode changed.");
    mode = next; controller.clear(); selected_window = 0; hide_cursors();
  }
  g_simple_action_set_state(action, value);
  if (mode == "separate") {
    auto* window = separate_window();
    if (!ephy_tab_view_get_n_pages(ephy_window_get_tab_view(window))) {
      gnosis_mcp_new_tab(GTK_WINDOW(window), "about:blank");
    }
    gtk_window_present(GTK_WINDOW(window));
  }
}
void validate(const std::string& name, JsonObject* args) {
  g_autoptr(GBytes) bytes = g_resources_lookup_data("/com/neg_zero/Gnosis/mcp/tools.json", G_RESOURCE_LOOKUP_FLAGS_NONE, nullptr);
  g_autoptr(JsonParser) parser = json_parser_new();
  gsize size = 0; const char* data = static_cast<const char*>(g_bytes_get_data(bytes, &size));
  if (!json_parser_load_from_data(parser, data, size, nullptr)) throw std::runtime_error("Missing MCP tool definitions.");
  auto* definitions = json_object_get_array_member(json_node_get_object(json_parser_get_root(parser)), "tools");
  for (guint i = 0; i < json_array_get_length(definitions); ++i) {
    auto* tool = json_array_get_object_element(definitions, i);
    if (name != json_object_get_string_member(tool, "name")) continue;
    auto* schema = json_object_get_object_member(tool, "inputSchema");
    auto* properties = json_object_get_object_member(schema, "properties");
    auto* required = json_object_get_array_member(schema, "required");
    for (guint j = 0; j < json_array_get_length(required); ++j)
      if (!json_object_has_member(args, json_array_get_string_element(required, j))) throw std::runtime_error("Missing required tool argument.");
    g_autoptr(GList) members = json_object_get_members(args);
    for (auto* member = members; member; member = member->next) {
      const char* key = static_cast<const char*>(member->data);
      if (!json_object_has_member(properties, key)) throw std::runtime_error(std::string("Unexpected argument: ") + key);
      auto* definition = json_object_get_object_member(properties, key);
      auto type = string(definition, "type", true);
      if (type == "string") string(args, key, true);
      else if (type == "boolean") boolean(args, key, false);
      else {
        auto value = number(args, key, 0);
        if ((json_object_has_member(definition, "minimum") && value < json_object_get_double_member(definition, "minimum")) ||
            (json_object_has_member(definition, "maximum") && value > json_object_get_double_member(definition, "maximum")))
          throw std::runtime_error(std::string("Number outside allowed range: ") + key);
      }
    }
    return;
  }
  throw std::runtime_error("Unknown browser tool.");
}
void execute(GDBusMethodInvocation* invocation, const std::string& name, JsonObject* args) {
  validate(name, args);
  if (name == "get_state") {
    g_autoptr(JsonObject) state = json_object_new();
    json_object_set_string_member(state, "mode", mode.c_str());
    json_object_set_boolean_member(state, "connected", !controller.empty());
    json_object_set_boolean_member(state, "busy", !pending.expired());
    reply(invocation, encode(state), false); return;
  }
  if (mode == "off") throw std::runtime_error("AI browsing is off. Choose Shared Tabs or Separate Window under AI Browsing in Gnosis's main menu.");
  const char* sender = g_dbus_method_invocation_get_sender(invocation);
  if (!controller.empty() && controller != sender) throw std::runtime_error("Another MCP client controls this browsing session. Turn AI browsing off to disconnect it.");
  if (auto current = pending.lock(); current && !current->done) throw std::runtime_error("A browser operation is already in progress.");
  controller = sender;
  auto request = std::make_shared<Request>(invocation);
  pending = request; request->start();
  try {
    if (name == "list_windows") {
      g_autoptr(JsonObject) result = json_object_new();
      auto* array = json_array_new();
      std::vector<EphyWindow*> seen;
      for (auto tab : tabs()) {
        if (std::find(seen.begin(), seen.end(), tab.window) != seen.end()) continue;
        seen.push_back(tab.window);
        json_array_add_object_element(array, window_info(tab.window));
      }
      json_object_set_array_member(result, "windows", array); request->success(result); return;
    }
    if (name == "activate_window") {
      auto* window = find_window(args);
      selected_window = ephy_window_get_uid(window);
      gtk_window_present(GTK_WINDOW(window));
      g_autoptr(JsonObject) result = window_info(window); request->success(result); return;
    }
    if (name == "list_tabs") {
      g_autoptr(JsonObject) result = json_object_new();
      auto* array = json_array_new();
      auto id = string(args, "window_id", false, 20);
      auto* window = id.empty() ? nullptr : find_window(args);
      for (auto tab : tabs()) if (!window || tab.window == window) json_array_add_object_element(array, tab_info(tab));
      json_object_set_array_member(result, "tabs", array); request->success(result); return;
    }
    if (name == "open_tab") {
      auto url = string(args, "url", false, 16384); if (url.empty()) url = "about:blank";
      if (!web_url(url)) throw std::runtime_error("Only http://, https:// and about:blank navigation is allowed.");
      auto* window = find_window(args);
      selected_window = ephy_window_get_uid(window);
      request->target_uri = url;
      request->view = EPHY_WEB_VIEW(g_object_ref(gnosis_mcp_new_tab(GTK_WINDOW(window), url.c_str())));
      request->loaded("", g_get_monotonic_time()); return;
    }
    auto tab = find_tab(args, true);
    request->view = EPHY_WEB_VIEW(g_object_ref(tab.view));
    request->preserve_document = name == "snapshot" || name == "click" || name == "move" || name == "type_text" || name == "scroll" || name == "press_key";
    request->document_epoch = ephy_engine_web_view_get_document_epoch(EPHY_ENGINE_VIEW(tab.view));
    if (request->preserve_document) {
      const char* current = ephy_engine_web_view_get_uri(EPHY_ENGINE_VIEW(tab.view));
      if (!current || !web_url(current)) throw std::runtime_error("Browser-internal and local-file pages cannot be controlled through MCP.");
    }
    if (name == "activate_tab") { g_autoptr(JsonObject) result = tab_info(tab); request->success(result); }
    else if (name == "close_tab") {
      ephy_tab_view_close(ephy_window_get_tab_view(tab.window), GTK_WIDGET(tab.embed));
      request->finish("{\"close_requested\":true}");
    } else if (name == "navigate" || name == "go_back" || name == "go_forward") {
      const char* current = ephy_engine_web_view_get_uri(EPHY_ENGINE_VIEW(tab.view));
      std::string before = current ? current : "";
      if (name == "navigate") {
        auto url = string(args, "url", true, 16384);
        if (!web_url(url)) throw std::runtime_error("Only http://, https:// and about:blank navigation is allowed.");
        request->target_uri = url;
        ephy_web_view_load_url(tab.view, url.c_str());
      } else if (name == "go_back") {
        if (!ephy_engine_web_view_can_go_back(EPHY_ENGINE_VIEW(tab.view))) throw std::runtime_error("No previous history entry.");
        ephy_engine_web_view_go_back(EPHY_ENGINE_VIEW(tab.view));
      } else {
        if (!ephy_engine_web_view_can_go_forward(EPHY_ENGINE_VIEW(tab.view))) throw std::runtime_error("No next history entry.");
        ephy_engine_web_view_go_forward(EPHY_ENGINE_VIEW(tab.view));
      }
      request->loaded(before, g_get_monotonic_time());
    } else if (name == "snapshot") {
      bool screenshot = boolean(args, "screenshot", true);
      g_autoptr(JsonObject) params = json_object_new(); json_object_set_string_member(params, "operation", "snapshot");
      request->script(params, [request, screenshot](JsonObject* info) {
        if (!screenshot) { request->success(info); return; }
        struct Snapshot { std::shared_ptr<Request> request; std::string text; };
        ephy_engine_web_view_get_snapshot(EPHY_ENGINE_VIEW(request->view), WEBKIT_SNAPSHOT_REGION_VISIBLE,
          WEBKIT_SNAPSHOT_OPTIONS_NONE, request->cancel, +[](GObject* source, GAsyncResult* result, gpointer data) {
            std::unique_ptr<Snapshot> snapshot(static_cast<Snapshot*>(data));
            auto request = snapshot->request;
            g_autoptr(GError) error = nullptr;
            g_autoptr(GdkTexture) texture = ephy_engine_web_view_get_snapshot_finish(EPHY_ENGINE_VIEW(source), result, &error);
            if (!request->valid()) return;
            if (error || !texture) { request->fail(error ? error->message : "No screenshot available."); return; }
            g_autoptr(GBytes) png = gdk_texture_save_to_png_bytes(texture);
            if (!png || g_bytes_get_size(png) > 16 * 1024 * 1024) { request->fail("Screenshot exceeds the 16 MiB limit."); return; }
            request->finish(snapshot->text, false, png);
          }, new Snapshot{request, encode(info)});
      });
    } else if (name == "click" || name == "move") {
      request->point(args, [request, name] {
        if (name == "click") request->click([request] { request->success(); });
        else request->mouse("mouseMoved", [request] { request->success(); });
      });
    } else if (name == "type_text") {
      auto text = string(args, "text", true);
      bool replace = boolean(args, "replace", false);
      auto insert = [request, text] {
        g_autoptr(JsonObject) params = json_object_new(); json_object_set_string_member(params, "text", text.c_str());
        request->command("Input.insertText", params, [request](JsonObject*) { request->success(); });
      };
      auto type = [request, insert, replace] {
        g_autoptr(JsonObject) params = json_object_new(); json_object_set_string_member(params, "operation", "editable");
        request->script(params, [request, insert, replace](JsonObject*) {
          if (replace) request->key("Control+A", insert); else insert();
        });
      };
      if (json_object_has_member(args, "ref")) request->point(args, [request, type] { request->click(type); });
      else type();
    } else if (name == "scroll") {
      double dx = number(args, "delta_x", 0, 10000), dy = number(args, "delta_y", 0, 10000);
      auto wheel = [request, dx, dy] {
        g_autoptr(JsonObject) params = json_object_new();
        json_object_set_string_member(params, "type", "mouseWheel");
        json_object_set_double_member(params, "x", request->x); json_object_set_double_member(params, "y", request->y);
        json_object_set_double_member(params, "deltaX", dx); json_object_set_double_member(params, "deltaY", dy);
        request->command("Input.dispatchMouseEvent", params, [request](JsonObject*) { request->success(); });
      };
      if (json_object_has_member(args, "x") || json_object_has_member(args, "y")) request->point(args, wheel);
      else {
        g_autoptr(JsonObject) params = json_object_new(); json_object_set_string_member(params, "operation", "snapshot");
        request->script(params, [request, wheel](JsonObject* info) {
          auto* viewport = json_object_get_object_member(info, "viewport");
          request->x = json_object_get_double_member(viewport, "width") / 2;
          request->y = json_object_get_double_member(viewport, "height") / 2; wheel();
        });
      }
    } else if (name == "press_key") request->key(string(args, "key", true, 64), [request] { request->success(); });
  } catch (const std::exception& error) { request->fail(error.what()); }
}
}

void gnosis_mcp_start(GApplication* app) {
  auto shell_mode = ephy_embed_shell_get_mode(EPHY_EMBED_SHELL(app));
  if (!GnosisBrave::enabled() || shell_mode == EPHY_EMBED_SHELL_MODE_INCOGNITO || shell_mode == EPHY_EMBED_SHELL_MODE_APPLICATION) return;
  connection = g_application_get_dbus_connection(app);
  if (!connection) return;
  application = app;
  g_autoptr(GDBusNodeInfo) info = g_dbus_node_info_new_for_xml(
    "<node><interface name='com.neg_zero.Gnosis.BrowserControl1'><method name='Execute'>"
    "<arg type='s' direction='in' name='tool'/><arg type='s' direction='in' name='arguments'/>"
    "<arg type='s' direction='out' name='result'/></method></interface></node>", nullptr);
  static const GDBusInterfaceVTable table = {+[](GDBusConnection*, const gchar*, const gchar*, const gchar*, const gchar*,
    GVariant* parameters, GDBusMethodInvocation* invocation, gpointer) {
      const char *tool, *json; g_variant_get(parameters, "(&s&s)", &tool, &json);
      try {
        if (strlen(json) > 1024 * 1024 || strlen(tool) > 64) throw std::runtime_error("Request is too large.");
        g_autoptr(JsonParser) parser = json_parser_new();
        if (!json_parser_load_from_data(parser, json, -1, nullptr) || !JSON_NODE_HOLDS_OBJECT(json_parser_get_root(parser)))
          throw std::runtime_error("Tool arguments must be a JSON object.");
        execute(invocation, tool, json_node_get_object(json_parser_get_root(parser)));
      } catch (const std::exception& error) { reply(invocation, error.what(), true); }
    }, nullptr, nullptr, {nullptr}};
  g_autoptr(GError) error = nullptr;
  registration = g_dbus_connection_register_object(connection, path, info->interfaces[0], &table, nullptr, nullptr, &error);
  if (!registration) { g_warning("AI browsing: %s", error->message); application = nullptr; connection = nullptr; return; }
  auto* action = g_simple_action_new_stateful("ai-browsing-mode", G_VARIANT_TYPE_STRING, g_variant_new_string("off"));
  g_signal_connect(action, "activate", G_CALLBACK(set_mode), nullptr);
  g_signal_connect(action, "change-state", G_CALLBACK(set_mode), nullptr);
  g_action_map_add_action(G_ACTION_MAP(app), G_ACTION(action)); g_object_unref(action);
  owner_watch = g_dbus_connection_signal_subscribe(connection, "org.freedesktop.DBus", "org.freedesktop.DBus",
    "NameOwnerChanged", "/org/freedesktop/DBus", nullptr, G_DBUS_SIGNAL_FLAGS_NONE,
    +[](GDBusConnection*, const gchar*, const gchar*, const gchar*, const gchar*, GVariant* params, gpointer) {
      const char *name, *old_owner, *new_owner; g_variant_get(params, "(&s&s&s)", &name, &old_owner, &new_owner);
      if (!controller.empty() && controller == name && !*new_owner) {
        ++generation; if (auto request = pending.lock()) request->fail("MCP client disconnected.");
        controller.clear(); selected_window = 0; hide_cursors();
      }
    }, nullptr, nullptr);
}
void gnosis_mcp_stop() {
  ++generation;
  if (auto request = pending.lock()) request->fail("The browser is shutting down.");
  hide_cursors(); controller.clear(); selected_window = 0; mode = "off";
  if (connection && owner_watch) g_dbus_connection_signal_unsubscribe(connection, owner_watch);
  if (connection && registration) g_dbus_connection_unregister_object(connection, registration);
  registration = owner_watch = 0; connection = nullptr; application = nullptr;
  if (ai_window) g_object_remove_weak_pointer(G_OBJECT(ai_window), reinterpret_cast<gpointer*>(&ai_window));
  ai_window = nullptr;
}
void gnosis_mcp_window_closed(GtkWindow* window) {
  if (window != GTK_WINDOW(ai_window)) return;
  g_object_remove_weak_pointer(G_OBJECT(ai_window), reinterpret_cast<gpointer*>(&ai_window));
  ai_window = nullptr;
  if (mode == "separate" && application) {
    auto* action = g_action_map_lookup_action(G_ACTION_MAP(application), "ai-browsing-mode");
    if (action) g_action_change_state(action, g_variant_new_string("off"));
  }
}
void gnosis_mcp_inherit_window(GtkWindow* window, GtkWindow* opener) {
  if (g_object_get_data(G_OBJECT(opener), ai_window_key))
    g_object_set_data(G_OBJECT(window), ai_window_key, GINT_TO_POINTER(1));
}
