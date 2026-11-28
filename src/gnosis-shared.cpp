/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "gnosis-shared.h"
#include "brave/browser.hpp"
#include "config.h"
#include "ephy-embed.h"
#include "ephy-window.h"
#include "gnosis-share-transport.hpp"
#include "gnosis-shared-ui.hpp"
#include <algorithm>
#include <cmath>
#include <json-glib/json-glib.h>
#include <map>
#include <memory>
#include <set>
#include <stdexcept>
#include <string>
#include <vector>

std::shared_ptr<GnosisBrave::Page>
ephy_engine_view_brave_page(EphyEngineView *);
namespace GnosisShare {
using GnosisShared::Interface;
namespace {
constexpr const char *viewer_uri = "ephy-about:shared-session";
constexpr const char *default_server = "wss://git.neg-zero.com/gnosis-session";
constexpr size_t max_packet = 4 * 1024 * 1024 - 2048;
std::string quote(const std::string &s) { return GnosisBrave::quote(s); }
std::string encode(JsonNode *n) {
  g_autofree char *s = json_to_string(n, false);
  return s ? s : "null";
}
std::string str(JsonObject *o, const char *key, size_t max = 65536) {
  auto *n = json_object_get_member(o, key);
  if (!n)
    return {};
  if (!JSON_NODE_HOLDS_VALUE(n) || json_node_get_value_type(n) != G_TYPE_STRING)
    throw std::runtime_error("Invalid text");
  std::string s = json_node_get_string(n);
  if (s.size() > max || !g_utf8_validate(s.data(), s.size(), nullptr))
    throw std::runtime_error("Text is too long");
  return s;
}
double num(JsonObject *o, const char *key, double fallback = 0,
           double max = 100000) {
  auto *n = json_object_get_member(o, key);
  if (!n)
    return fallback;
  auto t = json_node_get_value_type(n);
  if (t != G_TYPE_DOUBLE && t != G_TYPE_INT64)
    throw std::runtime_error("Invalid number");
  double v = json_node_get_double(n);
  if (!std::isfinite(v) || std::abs(v) > max)
    throw std::runtime_error("Invalid number");
  return v;
}
bool flag(JsonObject *o, const char *key, bool fallback = false) {
  auto *n = json_object_get_member(o, key);
  if (!n)
    return fallback;
  if (json_node_get_value_type(n) != G_TYPE_BOOLEAN)
    throw std::runtime_error("Invalid flag");
  return json_node_get_boolean(n);
}
bool web(const std::string &u) {
  return u.starts_with("https://") || u.starts_with("http://");
}
std::string name_valid(std::string s) {
  if (s.empty())
    return "Guest";
  if (s.size() > 64 || !g_utf8_validate(s.data(), s.size(), nullptr))
    throw std::runtime_error("Choose a shorter name");
  for (auto &c : s)
    if (static_cast<unsigned char>(c) < 32)
      c = ' ';
  return s;
}
std::string resource(const char *path) {
  g_autoptr(GBytes) b =
      g_resources_lookup_data(path, G_RESOURCE_LOOKUP_FLAGS_NONE, nullptr);
  if (!b)
    throw std::runtime_error("Shared session resources are unavailable");
  gsize n;
  auto *s = static_cast<const char *>(g_bytes_get_data(b, &n));
  return {s, n};
}
std::string script_tag(const std::string &js) {
  g_autofree char *b =
      g_base64_encode(reinterpret_cast<const guchar *>(js.data()), js.size());
  return "<script src=\"data:text/javascript;base64," + std::string(b) +
         "\"></script>";
}
struct Member {
  std::string name;
  bool approved = false, control = false, positioned = false;
  unsigned color = 1;
  int buttons = 0, resources = 0;
  double x = 0, y = 0;
};
struct Cached {
  std::string frame, mime;
};
struct Pending {
  std::string request, frame;
  gint64 deadline;
};
class Session;
GApplication *application = nullptr;
std::shared_ptr<Interface> interface;
std::shared_ptr<Session> current;

class Session : public std::enable_shared_from_this<Session> {
public:
  bool host = false, stopped = false, ready = false, viewer_ready = false,
       loading = false, capture_busy = false, metrics = false, resync = false,
       resize_pending = false;
  std::string name, id, host_id, frame, world, binding, rrweb, opaque;
  int context = 0, width = 1, height = 1;
  unsigned generation = 0;
  guint timer = 0;
  GWeakRef view{}, owner_window{};
  std::weak_ptr<GnosisBrave::Page> page;
  std::shared_ptr<Transport> transport;
  std::map<std::string, Member> members;
  std::map<std::string, Cached> cache;
  std::map<std::string, Pending> requests;
  std::vector<std::string> queued;
  size_t queued_bytes = 0;
  uint64_t request_serial = 0;
  explicit Session(bool hosting, std::string who)
      : host(hosting), name(name_valid(std::move(who))) {
    g_weak_ref_init(&view, nullptr);
    g_weak_ref_init(&owner_window, nullptr);
    g_autofree char *uuid = g_uuid_string_random();
    world = "gnosis-share-" + std::string(uuid);
    binding = "__gnosisShare";
  }
  ~Session() {
    stop();
    g_weak_ref_clear(&view);
    g_weak_ref_clear(&owner_window);
  }
  EphyEngineView *get_view() { return EPHY_ENGINE_VIEW(g_weak_ref_get(&view)); }
  void status(const std::string &text) {
    if (!stopped && interface)
      interface->status(text);
  }
  void setup(Invite invite) {
    transport = Transport::create(std::move(invite), host);
    auto weak = weak_from_this();
    transport->ready = [weak](const std::string &own,
                              const std::string &owner) {
      if (auto s = weak.lock(); s && !s->stopped) {
        s->id = own;
        s->host_id = owner;
        s->ready = true;
        if (!s->host)
          s->transport->send(
              owner, "{\"kind\":\"hello\",\"name\":" + quote(s->name) + "}");
        s->status(s->host ? "Waiting for someone to join"
                          : "Waiting for the host to allow you to join");
      }
    };
    transport->message = [weak](const std::string &from,
                                const std::string &json) {
      if (auto s = weak.lock(); s && !s->stopped)
        s->receive(from, json);
    };
    transport->status = [weak](const std::string &text) {
      if (auto s = weak.lock())
        s->status(text);
    };
    transport->failed = [weak](const std::string &text) {
      if (auto s = weak.lock(); s && !s->stopped) {
        s->stop();
        if (interface)
          interface->status(text);
      }
    };
    transport->left = [weak](const std::string &who) {
      if (auto s = weak.lock(); s && !s->stopped) {
        s->remove_cursor(who);
        s->members.erase(who);
        if (interface)
          interface->remove(who);
        s->broadcast("{\"kind\":\"left\",\"id\":" + quote(who) + "}");
      }
    };
    transport->connect();
    auto *hold = new std::weak_ptr<Session>(weak);
    timer = g_timeout_add_full(
        G_PRIORITY_DEFAULT, 100,
        +[](gpointer p) -> gboolean {
          auto s = static_cast<std::weak_ptr<Session> *>(p)->lock();
          if (!s || s->stopped)
            return G_SOURCE_REMOVE;
          s->tick();
          return s->stopped ? G_SOURCE_REMOVE : G_SOURCE_CONTINUE;
        },
        hold,
        +[](gpointer p) { delete static_cast<std::weak_ptr<Session> *>(p); });
  }
  void send(const std::string &to, const std::string &packet) {
    if (!stopped && transport && !transport->send(to, packet))
      resync = true;
  }
  void broadcast(const std::string &packet, const std::string &skip = {}) {
    for (const auto &[peer, m] : members)
      if (m.approved && peer != skip)
        send(peer, packet);
  }
  void viewer(const std::string &packet) {
    if (stopped)
      return;
    if (!viewer_ready) {
      if (queued_bytes + packet.size() > 8 * 1024 * 1024) {
        queued.clear();
        queued_bytes = 0;
        return;
      }
      queued.push_back(packet);
      queued_bytes += packet.size();
      return;
    }
    if (auto p = page.lock(); p && p->is_internal_page(viewer_uri)) {
      // This executes only in our trusted internal viewer, never in the
      // mirrored site.
      p->send_page_message(viewer_uri, "shared", packet);
    }
  }
  void attach(EphyEngineView *v) {
    g_weak_ref_set(&view, v);
    page = ephy_engine_view_brave_page(v);
  }
  EphyWindow *host_window() {
    g_autoptr(EphyEngineView) v = get_view();
    if (!v)
      return nullptr;
    auto *r = gtk_widget_get_root(GTK_WIDGET(v));
    return EPHY_IS_WINDOW(r) ? EPHY_WINDOW(r) : nullptr;
  }
  void remove_cursor(const std::string &peer) {
    g_autoptr(EphyWindow) w = EPHY_WINDOW(g_weak_ref_get(&owner_window));
    if (w)
      ephy_window_set_shared_cursor(w, nullptr, peer.c_str(), "", 0, 0, 0, 1,
                                    1);
  }
  void cursor(const std::string &peer, JsonObject *o) {
    auto it = members.find(peer);
    if (it == members.end() || !it->second.approved)
      return;
    double x = num(o, "x"), y = num(o, "y");
    if (x < 0 || y < 0 || x > width || y > height)
      return;
    it->second.x = x;
    it->second.y = y;
    it->second.positioned = true;
    g_autoptr(EphyEngineView) v = get_view();
    if (auto *w = host_window(); w && v)
      ephy_window_set_shared_cursor(w, EPHY_WEB_VIEW(v), peer.c_str(),
                                    it->second.name.c_str(), it->second.color,
                                    x, y, width, height);
    broadcast("{\"kind\":\"cursor\",\"id\":" + quote(peer) +
                  ",\"name\":" + quote(it->second.name) +
                  ",\"color\":" + std::to_string(it->second.color) + ",\"x\":" +
                  std::to_string(x) + ",\"y\":" + std::to_string(y) + "}",
              peer);
  }
  void approve(const std::string &peer) {
    auto it = members.find(peer);
    if (!host || stopped || it == members.end())
      return;
    it->second.approved = true;
    transport->allow(peer);
    interface->participant(peer, it->second.name, false, it->second.control);
    send(peer, "{\"kind\":\"grant\",\"control\":false,\"hostName\":" +
                   quote(name) + "}");
    if (context)
      snapshot();
    else
      record();
  }
  void kick(const std::string &peer) {
    if (!host || !transport)
      return;
    transport->kick(peer);
    remove_cursor(peer);
    members.erase(peer);
    interface->remove(peer);
  }
  void control(const std::string &peer, bool allowed) {
    auto it = members.find(peer);
    if (!host || it == members.end() || !it->second.approved)
      return;
    it->second.control = allowed;
    send(peer, "{\"kind\":\"grant\",\"control\":" +
                   std::string(allowed ? "true" : "false") + "}");
  }
  void snapshot() {
    if (auto p = page.lock(); p && context)
      p->command("Runtime.evaluate",
                 "{\"contextId\":" + std::to_string(context) +
                     ",\"expression\":\"globalThis.__gnosisSharedRecord?."
                     "snapshot()\"}");
    refresh_cache();
  }
  void refresh_cache(std::function<void()> done = {}) {
    auto p = page.lock();
    if (!p)
      return;
    auto weak = weak_from_this();
    unsigned gen = generation;
    p->command("Page.getResourceTree", "{}",
               [weak, gen, done = std::move(done)](JsonObject *o,
                                                   const std::string &error) {
                 if (auto s = weak.lock(); s && !s->stopped &&
                                           gen == s->generation && o &&
                                           error.empty()) {
                   s->cache.clear();
                   auto *n = json_object_get_member(o, "frameTree");
                   if (n && JSON_NODE_HOLDS_OBJECT(n))
                     s->cache_tree(json_node_get_object(n));
                   if (done)
                     done();
                 }
               });
  }
  void cache_tree(JsonObject *tree, unsigned depth = 0) {
    if (depth >= 32)
      return;
    auto *f = json_object_get_object_member(tree, "frame");
    if (!f)
      return;
    auto fid = str(f, "id");
    auto add = [&](JsonObject *r) {
      try {
        auto url = str(r, "url"), mime = str(r, "mimeType", 256);
        if (web(url) && cache.size() < 20000)
          cache[url] = {fid, mime};
      } catch (const std::exception &) {
      } // A site-controlled oversized resource URL is not fatal.
    };
    add(f);
    auto *rs = json_object_get_array_member(tree, "resources");
    if (rs)
      for (guint i = 0; i < json_array_get_length(rs); ++i) {
        auto *n = json_array_get_element(rs, i);
        if (JSON_NODE_HOLDS_OBJECT(n))
          add(json_node_get_object(n));
      }
    auto *children = json_object_get_array_member(tree, "childFrames");
    if (children)
      for (guint i = 0; i < json_array_get_length(children); ++i) {
        auto *n = json_array_get_element(children, i);
        if (JSON_NODE_HOLDS_OBJECT(n))
          cache_tree(json_node_get_object(n), depth + 1);
      }
  }
  void record() {
    if (stopped || loading)
      return;
    auto p = page.lock();
    if (!p || !web(p->uri))
      return;
    loading = true;
    auto weak = weak_from_this();
    unsigned gen = generation;
    if (rrweb.empty()) {
      auto *file = g_file_new_for_path(GNOSIS_RRWEB_FILE);
      auto *hold = new std::weak_ptr<Session>(weak);
      g_file_load_contents_async(
          file, nullptr,
          +[](GObject *f, GAsyncResult *r, gpointer data) {
            std::unique_ptr<std::weak_ptr<Session>> hold(
                static_cast<std::weak_ptr<Session> *>(data));
            auto s = hold->lock();
            g_autofree char *bytes = nullptr;
            gsize n = 0;
            g_autoptr(GError) error = nullptr;
            bool ok = g_file_load_contents_finish(G_FILE(f), r, &bytes, &n,
                                                  nullptr, &error);
            if (!s || s->stopped)
              return;
            s->loading = false;
            if (!ok || n > 8 * 1024 * 1024) {
              s->stop();
              interface->status("Could not load the page-sharing renderer");
              return;
            }
            s->rrweb.assign(bytes, n);
            s->record();
          },
          hold);
      g_object_unref(file);
      return;
    }
    p->sharing_event = [weak](const std::string &method, JsonObject *params) {
      if (auto s = weak.lock(); s && !s->stopped)
        s->host_event(method, params);
    };
    p->command(
        "Page.getFrameTree", "{}",
        [weak, gen](JsonObject *o, const std::string &err) {
          auto s = weak.lock();
          if (!s || s->stopped || gen != s->generation)
            return;
          auto p = s->page.lock();
          if (!p)
            return;
          if (!o || !err.empty()) {
            s->loading = false;
            return;
          }
          auto *t = json_object_get_object_member(o, "frameTree");
          auto *f = t ? json_object_get_object_member(t, "frame") : nullptr;
          if (!f) {
            s->loading = false;
            return;
          }
          s->frame = str(f, "id");
          p->command("Runtime.addBinding",
                     "{\"name\":" + quote(s->binding) +
                         ",\"executionContextName\":" + quote(s->world) + "}");
          p->command("Page.createIsolatedWorld",
                     "{\"frameId\":" + quote(s->frame) +
                         ",\"worldName\":" + quote(s->world) + "}",
                     [weak, gen](JsonObject *result, const std::string &error) {
                       auto s = weak.lock();
                       if (!s || s->stopped || gen != s->generation)
                         return;
                       s->loading = false;
                       if (!result || !error.empty())
                         return;
                       s->context =
                           static_cast<int>(num(result, "executionContextId"));
                       auto p = s->page.lock();
                       if (!p)
                         return;
                       try {
                         auto script =
                             s->rrweb + ";(" +
                             resource("/com/neg_zero/Gnosis/shared/record.js") +
                             ")(value=>globalThis[" + quote(s->binding) +
                             "](JSON.stringify(value)));";
                         p->command(
                             "Runtime.evaluate",
                             "{\"contextId\":" + std::to_string(s->context) +
                                 ",\"expression\":" + quote(script) + "}");
                         s->refresh_cache();
                       } catch (const std::exception &e) {
                         s->stop();
                         interface->status(e.what());
                       }
                     });
        });
  }
  void host_event(const std::string &method, JsonObject *o) {
    try {
      if (method == "Runtime.bindingCalled" && str(o, "name") == binding &&
          num(o, "executionContextId") == context) {
        auto payload = str(o, "payload", max_packet);
        auto *parser = json_parser_new();
        if (json_parser_load_from_data(parser, payload.data(), payload.size(),
                                       nullptr) &&
            JSON_NODE_HOLDS_OBJECT(json_parser_get_root(parser)))
          recorded(json_node_get_object(json_parser_get_root(parser)));
        g_object_unref(parser);
      } else if (method == "Page.frameNavigated") {
        auto *f = json_object_get_object_member(o, "frame");
        if (f && !json_object_has_member(f, "parentId")) {
          ++generation;
          context = 0;
          loading = false;
          cache.clear();
          if (!web(str(f, "url"))) {
            stop();
            interface->status(
                "Sharing stopped because this page cannot be shared");
          }
        }
      } else if (method == "Page.domContentEventFired" && !context)
        record();
    } catch (const std::exception &) {
    }
  }
  void recorded(JsonObject *o) {
    auto kind = str(o, "kind", 64);
    g_autoptr(JsonNode) node = json_node_new(JSON_NODE_OBJECT);
    json_node_set_object(node, o);
    if (kind == "event") {
      broadcast(encode(node));
      auto *e = json_object_get_object_member(o, "event");
      if (e && num(e, "type") == 2)
        refresh_cache();
    } else if (kind == "viewport") {
      width = std::clamp(static_cast<int>(num(o, "width")), 1, 8192);
      height = std::clamp(static_cast<int>(num(o, "height")), 1, 8192);
      broadcast(encode(node));
    } else if (kind == "cursor") {
      broadcast("{\"kind\":\"cursor\",\"id\":" + quote(id) +
                ",\"name\":" + quote(name) +
                ",\"color\":0,\"x\":" + std::to_string(num(o, "x")) +
                ",\"y\":" + std::to_string(num(o, "y")) + "}");
    } else if (kind == "opaque" && !capture_busy) {
      auto *n = json_object_get_member(o, "elements");
      if (!n || !JSON_NODE_HOLDS_ARRAY(n) ||
          json_array_get_length(json_node_get_array(n)) > 128)
        return;
      auto next = encode(n);
      if (json_array_get_length(json_node_get_array(n)) == 0) {
        if (opaque != "[]")
          broadcast(
              "{\"kind\":\"opaque\",\"image\":\"\",\"elements\":[],\"width\":" +
              std::to_string(width) + ",\"height\":" + std::to_string(height) +
              "}");
        opaque = "[]";
        return;
      }
      opaque = std::move(next);
      capture();
    }
  }
  void capture() {
    auto p = page.lock();
    if (!p)
      return;
    capture_busy = true;
    auto weak = weak_from_this();
    unsigned gen = generation;
    std::string rects = opaque;
    p->command(
        "Page.captureScreenshot",
        "{\"format\":\"jpeg\",\"quality\":65,\"captureBeyondViewport\":false}",
        [weak, gen, rects](JsonObject *o, const std::string &err) {
          if (auto s = weak.lock(); s && !s->stopped) {
            s->capture_busy = false;
            if (gen != s->generation || !o || !err.empty())
              return;
            try {
              auto image = str(o, "data", max_packet - 65536);
              s->broadcast("{\"kind\":\"opaque\",\"image\":" + quote(image) +
                           ",\"elements\":" + rects +
                           ",\"width\":" + std::to_string(s->width) +
                           ",\"height\":" + std::to_string(s->height) + "}");
            } catch (const std::exception &) {
            }
          }
        });
  }
  void host_resource(const std::string &peer, JsonObject *o,
                     bool refreshed = false) {
    auto token = str(o, "token", 64), url = str(o, "url");
    auto it = cache.find(url);
    if (it == cache.end()) {
      if (!refreshed) {
        auto weak = weak_from_this();
        auto args =
            std::shared_ptr<JsonObject>(json_object_ref(o), json_object_unref);
        refresh_cache([weak, peer, args] {
          if (auto s = weak.lock(); s && !s->stopped &&
                                    s->members.contains(peer) &&
                                    s->members[peer].approved)
            s->host_resource(peer, args.get(), true);
        });
        return;
      }
      send(peer, "{\"kind\":\"resource\",\"token\":" + quote(token) +
                     ",\"ok\":false}");
      return;
    }
    auto p = page.lock();
    if (!p)
      return;
    auto member = members.find(peer);
    if (member == members.end() || member->second.resources >= 32)
      return;
    ++member->second.resources;
    auto cached = it->second;
    auto weak = weak_from_this();
    unsigned gen = generation;
    p->command_for_frame(
        cached.frame, "Page.getResourceContent",
        "{\"frameId\":" + quote(cached.frame) + ",\"url\":" + quote(url) + "}",
        [weak, peer, token, cached, gen](JsonObject *o,
                                         const std::string &err) {
          auto s = weak.lock();
          if (!s || s->stopped)
            return;
          auto member = s->members.find(peer);
          if (member == s->members.end() || !member->second.approved)
            return;
          if (member->second.resources > 0)
            --member->second.resources;
          try {
            if (!o || !err.empty() || gen != s->generation)
              throw std::runtime_error("Missing resource");
            auto body = str(o, "content", max_packet / 2);
            if (!flag(o, "base64Encoded")) {
              g_autofree char *data = g_base64_encode(
                  reinterpret_cast<const guchar *>(body.data()), body.size());
              body = data;
            }
            s->send(peer, "{\"kind\":\"resource\",\"token\":" + quote(token) +
                              ",\"ok\":true,\"mime\":" + quote(cached.mime) +
                              ",\"body\":" + quote(body) + "}");
          } catch (const std::exception &) {
            s->send(peer, "{\"kind\":\"resource\",\"token\":" + quote(token) +
                              ",\"ok\":false}");
          }
        });
  }
  void guest_resource(JsonObject *o) {
    auto token = str(o, "token", 64);
    auto it = requests.find(token);
    if (it == requests.end())
      return;
    auto pending = it->second;
    auto p = page.lock();
    if (!p)
      return;
    if (!flag(o, "ok")) {
      requests.erase(token);
      p->command_for_frame(pending.frame, "Fetch.failRequest",
                           "{\"requestId\":" + quote(pending.request) +
                               ",\"errorReason\":\"AccessDenied\"}");
      return;
    }
    auto mime = str(o, "mime", 256), body = str(o, "body", max_packet);
    if (mime.find_first_of("\r\n") != std::string::npos)
      throw std::runtime_error("Invalid resource type");
    requests.erase(token);
    p->command_for_frame(
        pending.frame, "Fetch.fulfillRequest",
        "{\"requestId\":" + quote(pending.request) +
            ",\"responseCode\":200,\"responseHeaders\":[{\"name\":\"Content-"
            "Type\",\"value\":" +
            quote(mime) +
            "},{\"name\":\"Access-Control-Allow-Origin\",\"value\":\"*\"},{"
            "\"name\":\"Cache-Control\",\"value\":\"no-store\"}],\"body\":" +
            quote(body) + "}");
  }
  bool fetch(JsonObject *o) {
    auto *request = json_object_get_object_member(o, "request");
    auto p = page.lock();
    if (!request || !p)
      return false;
    auto url = str(request, "url");
    if (!web(url) || !web(p->canonical_uri(url)))
      return false;
    auto rid = str(o, "requestId"), fid = str(o, "frameId");
    if (requests.size() >= 128 || !ready) {
      p->command_for_frame(fid, "Fetch.failRequest",
                           "{\"requestId\":" + quote(rid) +
                               ",\"errorReason\":\"AccessDenied\"}");
      return true;
    }
    auto token = std::to_string(++request_serial);
    requests[token] = {rid, fid, g_get_monotonic_time() + 5000000};
    send(host_id, "{\"kind\":\"resource-request\",\"token\":" + quote(token) +
                      ",\"url\":" + quote(url) + "}");
    return true;
  }
  void receive(const std::string &from, const std::string &packet) {
    if (packet.size() > max_packet)
      return;
    g_autoptr(JsonParser) parser = json_parser_new();
    if (!json_parser_load_from_data(parser, packet.data(), packet.size(),
                                    nullptr) ||
        !JSON_NODE_HOLDS_OBJECT(json_parser_get_root(parser)))
      return;
    auto *o = json_node_get_object(json_parser_get_root(parser));
    try {
      auto kind = str(o, "kind", 64);
      if (host) {
        if (kind == "hello") {
          if (members.size() >= 7 && !members.contains(from))
            return;
          auto &m = members[from];
          if (m.name.empty()) {
            m.name = name_valid(str(o, "name", 64));
            m.color = 1 + members.size() % 5;
            interface->participant(from, m.name, true, false);
          }
          return;
        }
        auto it = members.find(from);
        if (it == members.end() || !it->second.approved)
          return;
        if (kind == "cursor") {
          cursor(from, o);
          if (it->second.control) {
            if (auto p = page.lock())
              p->command("Input.dispatchMouseEvent",
                         "{\"type\":\"mouseMoved\",\"x\":" +
                             std::to_string(num(o, "x")) + ",\"y\":" +
                             std::to_string(num(o, "y")) + ",\"buttons\":" +
                             std::to_string(it->second.buttons) + "}");
          }
        } else if (kind == "resource-request")
          host_resource(from, o);
        else if (kind == "snapshot-request")
          snapshot();
        else if (kind == "resize-request") {
          int w = num(o, "width"), h = num(o, "height");
          if (resize_pending || w < 100 || h < 100 || w > 8192 || h > 8192)
            return;
          resize_pending = true;
          auto weak = weak_from_this();
          interface->ask_resize(
              it->second.name, w, h, [weak, w, h, from](bool allow) {
                auto s = weak.lock();
                if (!s)
                  return;
                s->resize_pending = false;
                if (allow && !s->stopped && s->members.contains(from) &&
                    s->members[from].approved) {
                  if (auto p = s->page.lock()) {
                    s->metrics = true;
                    p->command(
                        "Emulation.setDeviceMetricsOverride",
                        "{\"width\":" + std::to_string(w) +
                            ",\"height\":" + std::to_string(h) +
                            ",\"deviceScaleFactor\":0,\"mobile\":false}");
                    s->snapshot();
                  }
                }
              });
        } else if (it->second.control)
          input(o, it->second);
      } else if (from == host_id) {
        if (kind == "grant") {
          status(flag(o, "control") ? "Connected — you can control this page"
                                    : "Connected — viewing only");
          viewer(packet);
        } else if (kind == "resource")
          guest_resource(o);
        else if (kind == "ended")
          stop();
        else if (kind == "event" || kind == "viewport" || kind == "cursor" ||
                 kind == "left" || kind == "opaque")
          viewer(packet);
      }
    } catch (const std::exception &) {
    }
  }
  void input(JsonObject *o, Member &member) {
    auto p = page.lock();
    if (!p)
      return;
    auto kind = str(o, "kind", 64);
    if (kind == "text") {
      p->command("Input.insertText",
                 "{\"text\":" + quote(str(o, "text")) + "}");
      return;
    }
    if (kind == "key") {
      auto key = str(o, "key", 64), code = str(o, "code", 64);
      int mods = static_cast<int>(num(o, "modifiers", 0, 15));
      static const std::map<std::string, int> keys = {
          {"ArrowLeft", 37}, {"ArrowRight", 39}, {"ArrowUp", 38},
          {"ArrowDown", 40}, {"Backspace", 8},   {"Delete", 46},
          {"Enter", 13},     {"Tab", 9},         {"Escape", 27},
          {"Home", 36},      {"End", 35},        {"PageUp", 33},
          {"PageDown", 34}};
      static const std::set<std::string> edit = {"a", "z", "y", "A", "Z", "Y"};
      if (mods & (1 | 4))
        return;
      int vk = 0;
      if (auto it = keys.find(key); it != keys.end()) {
        vk = it->second;
        if ((mods & 2) && (key == "Tab" || key == "Enter" || key == "Escape" ||
                           key == "PageUp" || key == "PageDown"))
          return;
      } else if ((mods & 2) && edit.contains(key))
        vk = g_ascii_toupper(key[0]);
      else
        return;
      auto params = ",\"key\":" + quote(key) + ",\"code\":" + quote(code) +
                    ",\"windowsVirtualKeyCode\":" + std::to_string(vk) +
                    ",\"modifiers\":" + std::to_string(mods);
      bool enter = key == "Enter" && !(mods & 2);
      p->command("Input.dispatchKeyEvent",
                 "{\"type\":" + quote(enter ? "keyDown" : "rawKeyDown") +
                     params + (enter ? ",\"text\":\"\\r\"" : "") + "}");
      p->command("Input.dispatchKeyEvent",
                 "{\"type\":\"keyUp\"" + params + "}");
      return;
    }
    if (kind != "mouse" && kind != "wheel")
      return;
    double x = num(o, "x"), y = num(o, "y");
    if (x < 0 || y < 0 || x > width || y > height)
      return;
    auto xy = ",\"x\":" + std::to_string(x) + ",\"y\":" + std::to_string(y);
    if (kind == "wheel") {
      auto dx = num(o, "deltaX", 0, 2000), dy = num(o, "deltaY", 0, 2000);
      p->command("Input.dispatchMouseEvent",
                 "{\"type\":\"mouseWheel\"" + xy +
                     ",\"deltaX\":" + std::to_string(dx) +
                     ",\"deltaY\":" + std::to_string(dy) + "}");
    } else {
      auto type = str(o, "type", 32), button = str(o, "button", 16);
      if ((type != "mousePressed" && type != "mouseReleased") ||
          (button != "left" && button != "middle" && button != "right"))
        return;
      int mask = button == "left" ? 1 : button == "right" ? 2 : 4;
      if (type == "mousePressed")
        member.buttons |= mask;
      else
        member.buttons &= ~mask;
      p->command("Input.dispatchMouseEvent",
                 "{\"type\":" + quote(type) + xy +
                     ",\"button\":" + quote(button) + ",\"buttons\":" +
                     std::to_string(member.buttons) + ",\"clickCount\":1}");
    }
  }
  void guest_message(JsonNode *n) {
    if (!n || !JSON_NODE_HOLDS_OBJECT(n) || stopped)
      return;
    auto *o = json_node_get_object(n);
    try {
      auto kind = str(o, "kind", 64);
      if (kind == "ready") {
        viewer_ready = true;
        auto backlog = std::move(queued);
        queued.clear();
        queued_bytes = 0;
        for (const auto &p : backlog)
          viewer(p);
        send(host_id, "{\"kind\":\"snapshot-request\"}");
      } else if (kind == "cursor" || kind == "mouse" || kind == "wheel" ||
                 kind == "text" || kind == "key" || kind == "resize-request")
        send(host_id, encode(n));
    } catch (const std::exception &) {
    }
  }
  void guest_init() {
    auto p = page.lock();
    if (!p || p->session.empty() || loading)
      return;
    loading = true;
    auto weak = weak_from_this();
    p->sharing_guest = true;
    p->command(
        "Target.setAutoAttach",
        R"({"autoAttach":true,"waitForDebuggerOnStart":true,"flatten":true,"filter":[{"type":"iframe","exclude":false},{"exclude":true}]})");
    p->sharing_fetch = [weak](JsonObject *o) {
      if (auto s = weak.lock(); s && !s->stopped)
        try {
          return s->fetch(o);
        } catch (const std::exception &) {
          if (auto p = s->page.lock()) {
            auto *n = json_object_get_member(o, "requestId");
            if (n && json_node_get_value_type(n) == G_TYPE_STRING)
              p->command("Fetch.failRequest",
                         "{\"requestId\":" + quote(json_node_get_string(n)) +
                             ",\"errorReason\":\"AccessDenied\"}");
          }
          return true;
        }
      return true;
    };
    p->command("Network.setCacheDisabled", "{\"cacheDisabled\":true}");
    p->command("Network.setBypassServiceWorker", "{\"bypass\":true}");
    p->command("Fetch.enable", "{\"patterns\":[{\"urlPattern\":\"http://"
                               "*\"},{\"urlPattern\":\"https://*\"}]}");
    g_autoptr(EphyEngineView) v = get_view();
    if (!v)
      return;
    ephy_engine_web_view_register_page_message_handler(v, "shared");
    auto *hold = new std::weak_ptr<Session>(weak);
    g_signal_connect_data(
        v, "page-message::shared",
        G_CALLBACK(+[](EphyEngineView *, JsonNode *n, gpointer data) {
          if (auto s = static_cast<std::weak_ptr<Session> *>(data)->lock())
            s->guest_message(n);
        }),
        hold,
        +[](gpointer p, GClosure *) {
          delete static_cast<std::weak_ptr<Session> *>(p);
        },
        G_CONNECT_DEFAULT);
    auto *file = g_file_new_for_path(GNOSIS_RRWEB_FILE);
    auto *ref = new std::weak_ptr<Session>(weak);
    g_file_load_contents_async(
        file, nullptr,
        +[](GObject *file, GAsyncResult *result, gpointer data) {
          std::unique_ptr<std::weak_ptr<Session>> hold(
              static_cast<std::weak_ptr<Session> *>(data));
          auto s = hold->lock();
          g_autofree char *bundle = nullptr;
          gsize n = 0;
          if (!g_file_load_contents_finish(G_FILE(file), result, &bundle, &n,
                                           nullptr, nullptr) ||
              !s || s->stopped)
            return;
          try {
            std::string html =
                "<!doctype html><meta name=viewport "
                "content='width=device-width,initial-scale=1'><title>Shared "
                "Session</"
                "title><style>html,body{margin:0;width:100%;height:100%;"
                "overflow:hidden;background:#000;color:white}#stage{position:"
                "absolute}#cursors{position:fixed;inset:0;pointer-events:none}<"
                "/style><div id=stage></div><div id=cursors></div><style>" +
                resource("/com/neg_zero/Gnosis/shared/rrweb.css") + "</style>" +
                script_tag(std::string(bundle, n)) +
                script_tag(
                    resource("/com/neg_zero/Gnosis/shared/geometry.js")) +
                script_tag(resource("/com/neg_zero/Gnosis/shared/view.js"));
            g_autoptr(EphyEngineView) v = s->get_view();
            if (v) {
              if (auto p = s->page.lock()) {
                auto fallback = p->internal_resource;
                p->internal_resource = [fallback, html = std::move(html)](
                                           const std::string &uri, bool main,
                                           GnosisBrave::ResourceReply reply) {
                  if (uri == viewer_uri && main) {
                    auto *bytes = g_bytes_new(html.data(), html.size());
                    reply(bytes, "text/html; charset=utf-8", {});
                    g_bytes_unref(bytes);
                  } else if (fallback)
                    fallback(uri, main, std::move(reply));
                  else
                    reply(nullptr, {}, "Resource unavailable");
                };
                ephy_engine_web_view_load_uri(v, viewer_uri);
              }
            }
          } catch (const std::exception &e) {
            s->stop();
            interface->status(e.what());
          }
        },
        ref);
    g_object_unref(file);
  }
  void tick() {
    g_autoptr(EphyEngineView) v = get_view();
    if (!v) {
      stop();
      return;
    }
    auto p = page.lock();
    if (!p) {
      p = ephy_engine_view_brave_page(v);
      page = p;
    }
    if (!p)
      return;
    if (p->is_closed()) {
      stop();
      return;
    }
    if (!host) {
      if (!loading)
        guest_init();
      auto now = g_get_monotonic_time();
      for (auto it = requests.begin(); it != requests.end();) {
        if (it->second.deadline < now) {
          auto r = it->second;
          p->command_for_frame(r.frame, "Fetch.failRequest",
                               "{\"requestId\":" + quote(r.request) +
                                   ",\"errorReason\":\"TimedOut\"}");
          it = requests.erase(it);
        } else
          ++it;
      }
    } else {
      g_autoptr(EphyWindow) previous =
          EPHY_WINDOW(g_weak_ref_get(&owner_window));
      auto *owner = host_window();
      if (owner != previous) {
        for (const auto &[peer, m] : members)
          remove_cursor(peer);
        g_weak_ref_set(&owner_window, owner);
        interface->attach_host(owner);
      }
      if (owner)
        for (const auto &[peer, m] : members)
          if (m.positioned)
            ephy_window_set_shared_cursor(owner, EPHY_WEB_VIEW(v), peer.c_str(),
                                          m.name.c_str(), m.color, m.x, m.y,
                                          width, height);
      if (resync) {
        resync = false;
        snapshot();
      }
    }
  }
  void stop() {
    if (stopped)
      return;
    stopped = true;
    if (timer) {
      g_source_remove(timer);
      timer = 0;
    }
    if (host && transport) {
      for (const auto &[peer, m] : members)
        if (m.approved)
          transport->send(peer, "{\"kind\":\"ended\"}");
    }
    if (transport)
      transport->close();
    if (auto p = page.lock()) {
      p->sharing_event = {};
      p->sharing_fetch = {};
      p->sharing_guest = false;
      if (host && context) {
        p->command(
            "Runtime.evaluate",
            "{\"contextId\":" + std::to_string(context) +
                ",\"expression\":\"globalThis.__gnosisSharedRecord?.stop()\"}");
        p->command("Runtime.removeBinding",
                   "{\"name\":" + quote(binding) + "}");
      }
      if (metrics)
        p->command("Emulation.clearDeviceMetricsOverride");
      if (!host) {
        for (const auto &[token, r] : requests)
          p->command_for_frame(r.frame, "Fetch.failRequest",
                               "{\"requestId\":" + quote(r.request) +
                                   ",\"errorReason\":\"Aborted\"}");
        p->navigate("about:blank");
      }
    }
    for (const auto &[peer, m] : members)
      remove_cursor(peer);
    requests.clear();
    queued.clear();
    cache.clear();
    if (interface)
      interface->clear();
  }
};

void stop_session() {
  if (current)
    current->stop();
  current.reset();
}
void start_host(const std::string &name, const std::string &server) {
  try {
    auto *active =
        gtk_application_get_active_window(GTK_APPLICATION(application));
    if (!EPHY_IS_WINDOW(active))
      throw std::runtime_error("Choose a browser window first");
    auto *embed = ephy_window_get_active_embed(EPHY_WINDOW(active));
    if (!embed)
      throw std::runtime_error("Open a page first");
    auto *v = EPHY_ENGINE_VIEW(ephy_embed_get_web_view(embed));
    EphyEngineSession *session = nullptr;
    g_object_get(v, "network-session", &session, nullptr);
    bool normal = session && !ephy_engine_session_is_ephemeral(session);
    g_clear_object(&session);
    auto p = ephy_engine_view_brave_page(v);
    if (!normal || !p || !web(p->uri))
      throw std::runtime_error(
          "Only regular HTTP and HTTPS tabs can be shared");
    auto invite = Invite::create(server.empty() ? default_server : server);
    stop_session();
    current = std::make_shared<Session>(true, name);
    current->attach(v);
    interface->active(true, p->title, invite.link());
    interface->attach_host(EPHY_WINDOW(active));
    interface->status("Connecting to the sharing server…");
    current->setup(std::move(invite));
  } catch (const std::exception &e) {
    interface->status(e.what());
  }
}
void start_guest(const std::string &invitation, const std::string &name) {
  try {
    auto invite = Invite::parse(invitation);
    stop_session();
    current = std::make_shared<Session>(false, name);
    interface->active(false, "Shared Session", {});
    auto *v = interface->create_guest();
    current->attach(v);
    interface->status("Connecting to the host…");
    current->setup(std::move(invite));
  } catch (const std::exception &e) {
    interface->status(e.what());
  }
}
} // namespace
} // namespace GnosisShare
extern "C" void gnosis_shared_start(GApplication *app) {
  using namespace GnosisShare;
  application = app;
  interface = Interface::create(GTK_APPLICATION(app));
  interface->host_requested = start_host;
  interface->join_requested = start_guest;
  interface->stop_requested = stop_session;
  interface->approved = [](const std::string &id) {
    if (current)
      current->approve(id);
  };
  interface->rejected = [](const std::string &id) {
    if (current)
      current->kick(id);
  };
  interface->removed = interface->rejected;
  interface->control_changed = [](const std::string &id, bool yes) {
    if (current)
      current->control(id, yes);
  };
  interface->zoom_changed = [](double factor) {
    if (current && !current->host)
      current->viewer(
          "{\"kind\":\"zoom\",\"factor\":" + std::to_string(factor) + "}");
  };
  interface->resize_requested = [](int w, int h) {
    if (current && !current->host)
      current->send(
          current->host_id,
          "{\"kind\":\"resize-request\",\"width\":" + std::to_string(w) +
              ",\"height\":" + std::to_string(h) + "}");
  };
  auto *action = g_simple_action_new("shared-sessions", nullptr);
  g_signal_connect(action, "activate",
                   G_CALLBACK(+[](GSimpleAction *, GVariant *, gpointer) {
                     interface->present(gtk_application_get_active_window(
                         GTK_APPLICATION(application)));
                   }),
                   nullptr);
  g_action_map_add_action(G_ACTION_MAP(app), G_ACTION(action));
  g_object_unref(action);
}
extern "C" void gnosis_shared_stop() {
  using namespace GnosisShare;
  stop_session();
  interface.reset();
  application = nullptr;
}
extern "C" gboolean gnosis_shared_open_uri(const char *uri) {
  using namespace GnosisShare;
  if (!uri || !g_str_has_prefix(uri, "gnosis-share:"))
    return false;
  if (interface) {
    start_guest(uri, g_get_real_name());
  }
  return true;
}
