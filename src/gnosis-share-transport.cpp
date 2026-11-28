/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "gnosis-share-transport.hpp"
#include <libsoup/soup.h>
#include <json-glib/json-glib.h>
#include <sodium.h>
#include <rtc/rtc.hpp>
#include <map>
#include <stdexcept>
#include <thread>
#include <cstring>
#include <algorithm>
#include <atomic>
#include <condition_variable>
#include <deque>
#include <mutex>
#include <limits>

namespace GnosisShare {
namespace {
constexpr size_t limit = 4*1024*1024;
void post(std::function<void()> fn) {
  g_idle_add_full(G_PRIORITY_DEFAULT,+[](gpointer p)->gboolean {
    (*static_cast<std::function<void()>*>(p))();return G_SOURCE_REMOVE;
  },new std::function<void()>(std::move(fn)),+[](gpointer p){delete static_cast<std::function<void()>*>(p);});
}
std::string quote(const std::string& str) {
  auto* n=json_node_new(JSON_NODE_VALUE);json_node_set_string(n,str.c_str());
  auto* p=json_to_string(n,false);std::string out=p;g_free(p);json_node_free(n);return out;
}
std::string encode(JsonNode* n) {auto* p=json_to_string(n,false);std::string out=p;g_free(p);return out;}
const char* text(JsonObject* obj,const char* key) {
  auto* n=json_object_get_member(obj,key);
  return n&&JSON_NODE_HOLDS_VALUE(n)&&json_node_get_value_type(n)==G_TYPE_STRING?json_node_get_string(n):"";
}
std::string b64(const unsigned char* data,size_t size) {
  std::string out(sodium_base64_ENCODED_LEN(size,sodium_base64_VARIANT_URLSAFE_NO_PADDING),0);
  sodium_bin2base64(out.data(),out.size(),data,size,sodium_base64_VARIANT_URLSAFE_NO_PADDING);
  out.resize(strlen(out.c_str()));return out;
}
std::string unbase64(const std::string& value) {
  if(value.size()>limit*2)throw std::runtime_error("Packet is too large");
  std::string out(value.size(),0);size_t n=0;
  if(sodium_base642bin(reinterpret_cast<unsigned char*>(out.data()),out.size(),value.data(),value.size(),nullptr,&n,nullptr,sodium_base64_VARIANT_URLSAFE_NO_PADDING))
    throw std::runtime_error("Invalid invitation or packet");
  out.resize(n);return out;
}
bool hex(const std::string& value) {return value.size()==32&&value.find_first_not_of("0123456789abcdef")==std::string::npos;}
void server_valid(const std::string& server) {
  auto* uri=g_uri_parse(server.c_str(),G_URI_FLAGS_NONE,nullptr);
  if(!uri)throw std::runtime_error("Invalid server address");
  const std::string scheme=g_uri_get_scheme(uri)?g_uri_get_scheme(uri):"",host=g_uri_get_host(uri)?g_uri_get_host(uri):"";
  bool valid=!host.empty()&&!g_uri_get_userinfo(uri)&&!g_uri_get_fragment(uri)&&!g_uri_get_query(uri)&&
    (scheme=="wss"||(scheme=="ws"&&(host=="localhost"||host=="127.0.0.1"||host=="::1")));
  g_uri_unref(uri);if(!valid)throw std::runtime_error("Use a secure wss:// server address");
}
}
Invite Invite::create(const std::string& server) {
  if(sodium_init()<0)throw std::runtime_error("Encryption is unavailable");server_valid(server);
  Invite i;i.server=server;while(i.server.ends_with('/'))i.server.pop_back();
  unsigned char room[16];randombytes_buf(room,sizeof room);char output[33];sodium_bin2hex(output,sizeof output,room,sizeof room);i.room=output;
  randombytes_buf(i.key.data(),i.key.size());return i;
}
std::string Invite::link() const {
  g_autofree char* encoded=g_uri_escape_string(server.c_str(),nullptr,false);
  return "gnosis-share://join/"+room+"?server="+encoded+"#"+b64(key.data(),key.size());
}
Invite Invite::parse(const std::string& link) {
  if(sodium_init()<0||link.size()>4096)throw std::runtime_error("Invalid invitation");
  auto* uri=g_uri_parse(link.c_str(),G_URI_FLAGS_NONE,nullptr);if(!uri)throw std::runtime_error("Invalid invitation");
  Invite i;bool valid=g_strcmp0(g_uri_get_scheme(uri),"gnosis-share")==0&&g_strcmp0(g_uri_get_host(uri),"join")==0&&!g_uri_get_userinfo(uri)&&g_uri_get_port(uri)==-1;
  const char* path=g_uri_get_path(uri);if(path&&*path=='/')i.room=path+1;
  auto* params=g_uri_parse_params(g_uri_get_query(uri)?g_uri_get_query(uri):"",-1,"&",G_URI_PARAMS_NONE,nullptr);
  if(params){auto* server=static_cast<char*>(g_hash_table_lookup(params,"server"));if(server)i.server=server;g_hash_table_unref(params);}
  std::string key=g_uri_get_fragment(uri)?g_uri_get_fragment(uri):"";g_uri_unref(uri);
  auto bytes=unbase64(key);if(!valid||!hex(i.room)||bytes.size()!=i.key.size())throw std::runtime_error("Invalid invitation");
  memcpy(i.key.data(),bytes.data(),bytes.size());server_valid(i.server);return i;
}

namespace detail {
constexpr size_t queued_limit = limit * 2;
constexpr size_t total_outgoing_limit = limit * 4;
constexpr size_t packet_count_limit = 128;

// All libdatachannel construction, negotiation, sends and teardown stay off GTK's thread.
struct Worker : std::enable_shared_from_this<Worker> {
  std::mutex mutex;
  std::condition_variable changed;
  std::deque<std::pair<size_t,std::function<void(Worker&)>>> queue;
  size_t bytes = 0;
  bool stopping = false;
  std::atomic<bool> direct{false};
  std::shared_ptr<rtc::PeerConnection> pc;
  std::shared_ptr<rtc::DataChannel> dc;
  static std::shared_ptr<Worker> create() {
    auto worker = std::make_shared<Worker>();
    std::thread([worker] {
      for (;;) {
        std::function<void(Worker&)> task;
        {
          std::unique_lock lock(worker->mutex);
          worker->changed.wait(lock,[&] { return worker->stopping || !worker->queue.empty(); });
          if (worker->stopping) { worker->queue.clear();worker->bytes=0;break; }
          auto item = std::move(worker->queue.front());
          worker->queue.pop_front(); worker->bytes -= item.first; task = std::move(item.second);
        }
        try { task(*worker); } catch (const std::exception&) {}
      }
      try {
        if (worker->dc) { worker->dc->resetCallbacks(); worker->dc->close(); worker->dc.reset(); }
        if (worker->pc) { worker->pc->resetCallbacks(); worker->pc->close(); worker->pc.reset(); }
      } catch (const std::exception&) {}
    }).detach();
    return worker;
  }
  bool submit(std::function<void(Worker&)> task,size_t size = 0) {
    std::lock_guard lock(mutex);
    if (stopping || queue.size() >= 256 || size > queued_limit - bytes) return false;
    bytes += size; queue.emplace_back(size,std::move(task)); changed.notify_one(); return true;
  }
  void stop() {
    std::lock_guard lock(mutex);
    // Queued channels can own their final reference; release them on the worker too.
    stopping = true; direct = false; changed.notify_one();
  }
};
struct Inbox {
  std::atomic<size_t> bytes{0},count{0};
  std::atomic<bool> overflow{false};
};
}
using detail::Worker;
using detail::Inbox;
using detail::queued_limit;
using detail::total_outgoing_limit;
using detail::packet_count_limit;
struct Transport::State {
  struct Outgoing { std::string box; gint64 since; bool relayed = false,queued = false; };
  struct Lane {
    uint64_t sent = 0,next = 1;
    bool ack = false;
    std::map<uint64_t,Outgoing> outgoing;
    std::map<uint64_t,std::string> waiting;
  };
  struct Peer {
    bool approved = false;
    std::shared_ptr<Worker> worker;
    std::array<Lane,2> lanes;
    size_t outgoing_bytes = 0,waiting_bytes = 0;
    ~Peer() { if (worker) worker->stop(); }
  };
  Invite invite;
  bool host = false,closed = false,connecting = false;
  SoupSession* soup = nullptr;
  SoupWebsocketConnection* ws = nullptr;
  GCancellable* cancel = g_cancellable_new();
  guint timer = 0;
  size_t outgoing_bytes = 0;
  std::string id,host_id;
  std::shared_ptr<Inbox> inbox = std::make_shared<Inbox>();
  std::map<std::string,std::shared_ptr<Peer>> peers;
};
Transport::Transport(Invite invite,bool host):s(std::make_unique<State>()) { s->invite=std::move(invite);s->host=host; }
std::shared_ptr<Transport> Transport::create(Invite invite,bool host) { return std::shared_ptr<Transport>(new Transport(std::move(invite),host)); }
Transport::~Transport() { close();g_clear_object(&s->cancel);sodium_memzero(s->invite.key.data(),s->invite.key.size()); }
const Invite& Transport::invitation() const { return s->invite; }
void Transport::fail(const std::string& reason) {
  if (s->closed) return;
  auto callback = failed; close(); if (callback) callback(reason);
}
void Transport::connect() {
  if (s->closed || s->connecting) return;
  s->connecting = true;
  s->soup = soup_session_new();
  auto* request = soup_message_new("GET",(s->invite.server+"/session").c_str());
  if (!request) { fail("Invalid server address");return; }
  auto* weak = new std::weak_ptr<Transport>(weak_from_this());
  soup_session_websocket_connect_async(s->soup,request,nullptr,nullptr,G_PRIORITY_DEFAULT,s->cancel,
    +[](GObject* object,GAsyncResult* result,gpointer pointer) {
      std::unique_ptr<std::weak_ptr<Transport>> weak(static_cast<std::weak_ptr<Transport>*>(pointer));
      auto self = weak->lock();
      g_autoptr(GError) error = nullptr;
      auto* ws = soup_session_websocket_connect_finish(SOUP_SESSION(object),result,&error);
      if (!self || self->s->closed) { g_clear_object(&ws);return; }
      if (!ws) { self->fail("Could not connect to the session server");return; }
      self->s->ws = ws;
      soup_websocket_connection_set_max_incoming_payload_size(ws,limit*2);
      soup_websocket_connection_set_keepalive_interval(ws,20);
      g_signal_connect(ws,"message",G_CALLBACK(+[](SoupWebsocketConnection*,gint type,GBytes* bytes,gpointer pointer) {
        auto self = static_cast<Transport*>(pointer)->shared_from_this();
        if (self->s->closed || type != SOUP_WEBSOCKET_DATA_TEXT) return;
        gsize size;
        auto* data = static_cast<const char*>(g_bytes_get_data(bytes,&size));
        g_autoptr(JsonParser) parser = json_parser_new();
        if (size > limit*2 || !json_parser_load_from_data(parser,data,size,nullptr) ||
            !JSON_NODE_HOLDS_OBJECT(json_parser_get_root(parser))) return;
        auto* object = json_node_get_object(json_parser_get_root(parser));
        std::string op = text(object,"op");
        if (op == "welcome" && self->s->id.empty()) {
          self->s->id = text(object,"id");self->s->host_id = text(object,"host");
          if (!hex(self->s->id) || !hex(self->s->host_id) || self->s->host != (self->s->id == self->s->host_id)) {
            self->fail("Invalid session response");return;
          }
          auto callback = self->ready;
          if (callback) callback(self->s->id,self->s->host_id);
        } else if (op == "relay") {
          try { self->receive(text(object,"from"),unbase64(text(object,"box"))); } catch (const std::exception&) {}
        } else if (op == "allowed" && !self->s->host && !self->s->host_id.empty()) {
          auto& slot = self->s->peers[self->s->host_id];
          if (!slot) slot = std::make_shared<State::Peer>();
          slot->approved = true;
          for (auto& lane : slot->lanes) for (auto& [_,outgoing] : lane.outgoing) outgoing.since = g_get_monotonic_time();
          self->peer(self->s->host_id,false);
          auto callback = self->status;
          if (callback) callback("Connected through relay");
        } else if (op == "left") {
          std::string id = text(object,"id");
          if (self->s->peers.contains(id)) {
            self->drop(id);
            auto callback = self->left;
            if (callback) callback(id);
          }
        }
      }),self.get());
      g_signal_connect(ws,"closed",G_CALLBACK(+[](SoupWebsocketConnection*,gpointer pointer) {
        auto self = static_cast<Transport*>(pointer)->shared_from_this();
        if (!self->s->closed) self->fail("The sharing session ended");
      }),self.get());
      self->websocket("{\"op\":"+quote(self->s->host?"create":"join")+",\"room\":"+quote(self->s->invite.room)+"}");
    },weak);
  g_object_unref(request);
  auto* timer_weak = new std::weak_ptr<Transport>(weak_from_this());
  s->timer = g_timeout_add_full(G_PRIORITY_DEFAULT,50,+[](gpointer pointer)->gboolean {
    auto self = static_cast<std::weak_ptr<Transport>*>(pointer)->lock();
    if (!self || self->s->closed) return G_SOURCE_REMOVE;
    self->tick();return self->s->closed?G_SOURCE_REMOVE:G_SOURCE_CONTINUE;
  },timer_weak,+[](gpointer pointer) { delete static_cast<std::weak_ptr<Transport>*>(pointer); });
}
bool Transport::websocket(const std::string& packet) {
  if (s->closed || !s->ws || soup_websocket_connection_get_state(s->ws) != SOUP_WEBSOCKET_STATE_OPEN) return false;
  soup_websocket_connection_send_text(s->ws,packet.c_str());return true;
}
void Transport::allow(const std::string& id) {
  if (s->closed || !s->host || !hex(id) || !s->peers.contains(id)) return;
  auto peer = s->peers.at(id);
  if (peer->approved) return;
  if (!websocket("{\"op\":\"allow\",\"to\":"+quote(id)+"}")) return;
  peer->approved = true;
  acknowledge(id);
  this->peer(id,true);
}
void Transport::drop(const std::string& id) {
  auto found = s->peers.find(id);
  if (found == s->peers.end()) return;
  s->outgoing_bytes -= found->second->outgoing_bytes;
  if (found->second->worker) found->second->worker->stop();
  s->peers.erase(found);
}
void Transport::kick(const std::string& id) {
  if (s->closed || !s->host || !hex(id)) return;
  websocket("{\"op\":\"kick\",\"to\":"+quote(id)+"}");drop(id);
}
bool Transport::send(const std::string& id,const std::string& json) { return packet(id,json,false); }
bool Transport::packet(const std::string& id,const std::string& json,bool signal) {
  if (s->closed || s->id.empty() || !hex(id) || id == s->id || (!s->host && id != s->host_id) ||
      json.size() > limit-1024 || !s->ws || soup_websocket_connection_get_state(s->ws) != SOUP_WEBSOCKET_STATE_OPEN) return false;
  if (!s->peers.contains(id) && (s->host || s->peers.size() >= 8)) return false;
  auto& slot = s->peers[id];
  if (!slot) slot = std::make_shared<State::Peer>();
  auto peer = slot;
  if (!peer->approved && (s->host || signal || json.size() > 2048 || !peer->lanes[0].outgoing.empty())) return false;
  g_autoptr(JsonParser) parser = json_parser_new();
  if (!json_parser_load_from_data(parser,json.data(),json.size(),nullptr) || !JSON_NODE_HOLDS_OBJECT(json_parser_get_root(parser))) return false;
  auto& lane = peer->lanes[signal?1:0];
  if (lane.sent >= static_cast<uint64_t>(std::numeric_limits<int64_t>::max()) || lane.outgoing.size() >= packet_count_limit) return false;
  const auto sequence = lane.sent + 1;
  auto plain = "{\"from\":"+quote(s->id)+",\"to\":"+quote(id)+",\"seq\":"+std::to_string(sequence)+
    ",\"signal\":"+(signal?"true":"false")+",\"data\":"+json+"}";
  std::string box(crypto_secretbox_NONCEBYTES+crypto_secretbox_MACBYTES+plain.size(),0);
  if (box.size() > limit || box.size() > queued_limit-peer->outgoing_bytes || box.size() > total_outgoing_limit-s->outgoing_bytes) return false;
  auto* bytes = reinterpret_cast<unsigned char*>(box.data());randombytes_buf(bytes,crypto_secretbox_NONCEBYTES);
  crypto_secretbox_easy(bytes+crypto_secretbox_NONCEBYTES,reinterpret_cast<const unsigned char*>(plain.data()),plain.size(),bytes,s->invite.key.data());
  // DNS/certificate/ICE setup can take seconds; relay stays usable throughout it.
  bool queued = !signal && peer->worker && peer->worker->direct.load();
  if (queued) {
    auto weak = weak_from_this();std::weak_ptr<State::Peer> peer_weak = peer;
    if (!peer->worker->submit([weak,peer_weak,id,sequence,box](Worker& worker) {
      bool direct = false;
      if (worker.dc && worker.dc->isOpen() && box.size() <= worker.dc->maxMessageSize() &&
          worker.dc->bufferedAmount()+box.size() <= queued_limit) {
        try { worker.dc->send(reinterpret_cast<const std::byte*>(box.data()),box.size());direct=true; } catch (const std::exception&) {}
      }
      post([weak,peer_weak,id,sequence,box,direct] {
        auto self = weak.lock();auto peer = peer_weak.lock();
        if (!self || self->s->closed || !peer || !self->s->peers.contains(id) || self->s->peers.at(id) != peer) return;
        auto found = peer->lanes[0].outgoing.find(sequence);
        if (found == peer->lanes[0].outgoing.end()) return;
        found->second.queued = false;
        if (!direct) {
          found->second.relayed = true;
          self->websocket("{\"op\":\"relay\",\"to\":"+quote(id)+",\"box\":"+quote(b64(reinterpret_cast<const unsigned char*>(box.data()),box.size()))+"}");
        }
      });
    },box.size())) return false;
  }
  lane.sent = sequence;peer->outgoing_bytes += box.size();s->outgoing_bytes += box.size();
  lane.outgoing.emplace(sequence,State::Outgoing{box,g_get_monotonic_time(),!queued,queued});
  if (!queued) websocket("{\"op\":\"relay\",\"to\":"+quote(id)+",\"box\":"+quote(b64(bytes,box.size()))+"}");
  return true;
}
void Transport::acknowledge(const std::string& id) {
  auto found = s->peers.find(id);
  if (found == s->peers.end() || !found->second->approved) return;
  for (size_t index=0;index<2;++index) {
    auto& lane = found->second->lanes[index];
    if (!lane.ack || lane.next == 1) continue;
    auto plain = "{\"from\":"+quote(s->id)+",\"to\":"+quote(id)+",\"ack\":"+std::to_string(lane.next-1)+",\"signal\":"+(index?"true":"false")+"}";
    std::string box(40+plain.size(),0);
    auto* bytes = reinterpret_cast<unsigned char*>(box.data());randombytes_buf(bytes,24);
    crypto_secretbox_easy(bytes+24,reinterpret_cast<const unsigned char*>(plain.data()),plain.size(),bytes,s->invite.key.data());
    if (websocket("{\"op\":\"relay\",\"to\":"+quote(id)+",\"box\":"+quote(b64(bytes,box.size()))+"}")) lane.ack = false;
  }
}
void Transport::receive(const std::string& id,const std::string& box) {
  if (s->closed || s->id.empty() || !hex(id) || id == s->id || (!s->host && id != s->host_id) || box.size()<40 || box.size()>limit) return;
  auto* bytes = reinterpret_cast<const unsigned char*>(box.data());std::string plain(box.size()-40,0);
  if (crypto_secretbox_open_easy(reinterpret_cast<unsigned char*>(plain.data()),bytes+24,box.size()-24,bytes,s->invite.key.data())) return;
  g_autoptr(JsonParser) parser = json_parser_new();
  if (!json_parser_load_from_data(parser,plain.data(),plain.size(),nullptr) || !JSON_NODE_HOLDS_OBJECT(json_parser_get_root(parser))) return;
  auto* object = json_node_get_object(json_parser_get_root(parser));
  auto* signal_node = json_object_get_member(object,"signal");
  if (id != text(object,"from") || s->id != text(object,"to") || !signal_node || json_node_get_value_type(signal_node) != G_TYPE_BOOLEAN) return;
  bool signal = json_node_get_boolean(signal_node);
  auto found = s->peers.find(id);
  auto* ack = json_object_get_member(object,"ack");
  if (ack) {
    if (found == s->peers.end() || json_node_get_value_type(ack) != G_TYPE_INT64 || json_node_get_int(ack) <= 0) return;
    auto peer = found->second;auto& lane = peer->lanes[signal?1:0];
    const auto acknowledged = static_cast<uint64_t>(json_node_get_int(ack));
    if (acknowledged > lane.sent) return;
    while (!lane.outgoing.empty() && lane.outgoing.begin()->first <= acknowledged) {
      size_t size = lane.outgoing.begin()->second.box.size();peer->outgoing_bytes -= size;s->outgoing_bytes -= size;
      lane.outgoing.erase(lane.outgoing.begin());
    }
    return;
  }
  auto* serial = json_object_get_member(object,"seq");auto* data = json_object_get_member(object,"data");
  if (!serial || json_node_get_value_type(serial) != G_TYPE_INT64 || json_node_get_int(serial) <= 0 || !data || !JSON_NODE_HOLDS_OBJECT(data)) return;
  if (found == s->peers.end()) {
    if (!s->host || signal || s->peers.size() >= 8 || plain.size() > 4096) return;
    found = s->peers.emplace(id,std::make_shared<State::Peer>()).first;
  }
  auto peer = found->second;
  if (!peer->approved && (signal || plain.size() > 4096 || json_node_get_int(serial) != 1)) return;
  auto& lane = peer->lanes[signal?1:0];auto sequence = static_cast<uint64_t>(json_node_get_int(serial));
  if (sequence < lane.next) { lane.ack = true;return; }
  if (sequence > lane.next + packet_count_limit || peer->waiting_bytes + plain.size() > queued_limit) return;
  if (lane.waiting.emplace(sequence,plain).second) peer->waiting_bytes += plain.size();
  while (!s->closed && s->peers.contains(id) && s->peers.at(id) == peer && lane.waiting.contains(lane.next)) {
    auto payload = std::move(lane.waiting.at(lane.next));lane.waiting.erase(lane.next++);peer->waiting_bytes -= payload.size();lane.ack = true;
    g_autoptr(JsonParser) current = json_parser_new();
    json_parser_load_from_data(current,payload.data(),payload.size(),nullptr);
    auto* root = json_node_get_object(json_parser_get_root(current));
    auto* value = json_object_get_object_member(root,"data");
    if (signal && peer->worker) {
      const std::string kind = text(value,"kind"),sdp = text(value,"sdp"),type = text(value,"type"),candidate = text(value,"candidate"),mid = text(value,"mid");
      peer->worker->submit([kind,sdp,type,candidate,mid](Worker& worker) {
        if (!worker.pc) return;
        if (kind == "sdp" && sdp.size() <= 65536 && (type == "offer" || type == "answer")) worker.pc->setRemoteDescription(rtc::Description(sdp,type));
        else if (kind == "ice" && candidate.size() <= 4096 && mid.size() <= 256) worker.pc->addRemoteCandidate(rtc::Candidate(candidate,mid));
      },payload.size());
    } else if (!signal) {
      auto callback = message;
      if (callback) callback(id,encode(json_object_get_member(root,"data")));
    }
  }
}
void Transport::tick() {
  const auto now = g_get_monotonic_time();
  std::vector<std::string> expired;
  for (const auto& [id,peer] : s->peers) {
    acknowledge(id);
    if (!peer->approved) continue;
    for (auto& lane : peer->lanes) for (auto& [_,outgoing] : lane.outgoing) {
      if (now - outgoing.since > 30000000) { expired.push_back(id);break; }
      // Relay is reliable. Only an unacknowledged direct send needs transition recovery.
      if (!outgoing.relayed && !outgoing.queued && now - outgoing.since > 1500000) {
        websocket("{\"op\":\"relay\",\"to\":"+quote(id)+",\"box\":"+quote(b64(reinterpret_cast<const unsigned char*>(outgoing.box.data()),outgoing.box.size()))+"}");
        outgoing.relayed = true;
      }
    }
  }
  for (const auto& id : expired) {
    if (!s->host) { fail("The host stopped responding");return; }
    kick(id);auto callback = left;if (callback) callback(id);
  }
}
void Transport::peer(const std::string& id,bool offer) {
  if (s->closed || !s->peers.contains(id) || !s->peers.at(id)->approved || g_getenv("GNOSIS_SHARE_DISABLE_P2P")) return;
  auto peer = s->peers.at(id);if (peer->worker) return;
  auto worker = Worker::create();peer->worker = worker;
  auto weak = weak_from_this();std::weak_ptr<State::Peer> peer_weak = peer;std::weak_ptr<Worker> worker_weak = worker;
  auto inbox = s->inbox;
  worker->submit([weak,peer_weak,worker_weak,inbox,id,offer](Worker& worker) {
    rtc::Configuration config;
    const char* stun = g_getenv("GNOSIS_SHARE_STUN_SERVER");
    if (!stun) config.iceServers.emplace_back("stun:stun.l.google.com:19302");
    else if (*stun) config.iceServers.emplace_back(stun);
    config.maxMessageSize = limit;config.enableIceTcp = true;
    worker.pc = std::make_shared<rtc::PeerConnection>(config);
    worker.pc->onLocalDescription([weak,peer_weak,id](rtc::Description description) {
      auto sdp = std::string(description),type = description.typeString();
      post([weak,peer_weak,id,sdp,type] {
        auto self = weak.lock();auto peer = peer_weak.lock();
        if (self && !self->s->closed && peer && self->s->peers.contains(id) && self->s->peers.at(id) == peer)
          self->packet(id,"{\"kind\":\"sdp\",\"sdp\":"+quote(sdp)+",\"type\":"+quote(type)+"}",true);
      });
    });
    worker.pc->onLocalCandidate([weak,peer_weak,id](rtc::Candidate candidate) {
      auto value = std::string(candidate),mid = candidate.mid();
      post([weak,peer_weak,id,value,mid] {
        auto self = weak.lock();auto peer = peer_weak.lock();
        if (self && !self->s->closed && peer && self->s->peers.contains(id) && self->s->peers.at(id) == peer)
          self->packet(id,"{\"kind\":\"ice\",\"candidate\":"+quote(value)+",\"mid\":"+quote(mid)+"}",true);
      });
    });
    auto channel = [weak,peer_weak,worker_weak,inbox,id](std::shared_ptr<rtc::DataChannel> dc) {
      auto worker = worker_weak.lock();if (!worker) return;
      worker->submit([weak,peer_weak,worker_weak,inbox,id,dc](Worker& worker) {
        if (worker.dc) return;
        if (dc->label() != "gnosis-session" || dc->protocol() != "") { dc->close();return; }
        worker.dc = dc;
        auto opened = [weak,peer_weak,worker_weak,id] {
          if (auto worker = worker_weak.lock()) worker->direct = true;
          post([weak,peer_weak,id] {
            auto self = weak.lock();auto peer = peer_weak.lock();
            if (self && !self->s->closed && peer && self->s->peers.contains(id) && self->s->peers.at(id) == peer) {
              auto callback = self->status;if (callback) callback("Connected peer-to-peer");
            }
          });
        };
        dc->onOpen(opened);
        dc->onClosed([weak,peer_weak,worker_weak,id] {
          if (auto worker = worker_weak.lock()) worker->direct = false;
          post([weak,peer_weak,id] {
            auto self = weak.lock();auto peer = peer_weak.lock();
            if (self && !self->s->closed && peer && self->s->peers.contains(id) && self->s->peers.at(id) == peer) {
              auto callback = self->status;if (callback) callback("Connected through relay");
            }
          });
        });
        dc->onMessage([weak,peer_weak,inbox,id](rtc::message_variant value) {
          if (!std::holds_alternative<rtc::binary>(value)) return;
          auto& bytes = std::get<rtc::binary>(value);size_t size = bytes.size();
          if (size > limit) return;
          auto old_bytes = inbox->bytes.fetch_add(size);auto old_count = inbox->count.fetch_add(1);
          if (old_bytes+size > queued_limit || old_count >= 256) {
            inbox->bytes.fetch_sub(size);inbox->count.fetch_sub(1);
            if (!inbox->overflow.exchange(true)) post([weak,peer_weak,inbox,id] {
              inbox->overflow = false;auto self = weak.lock();auto peer = peer_weak.lock();
              if (self && !self->s->closed && peer && self->s->peers.contains(id) && self->s->peers.at(id) == peer) {
                if (self->s->host) { self->kick(id);auto callback = self->left;if (callback) callback(id); }
                else self->fail("The session exceeded its receive limit");
              }
            });
            return;
          }
          std::string box(reinterpret_cast<const char*>(bytes.data()),size);
          post([weak,peer_weak,inbox,id,size,box=std::move(box)] {
            inbox->bytes.fetch_sub(size);inbox->count.fetch_sub(1);
            auto self = weak.lock();auto peer = peer_weak.lock();
            if (self && !self->s->closed && peer && self->s->peers.contains(id) && self->s->peers.at(id) == peer) self->receive(id,box);
          });
        });
        if (dc->isOpen()) opened();
      });
    };
    worker.pc->onDataChannel(channel);
    if (offer) channel(worker.pc->createDataChannel("gnosis-session"));
  });
}
void Transport::close() {
  if (!s || s->closed) return;
  s->closed = true;g_cancellable_cancel(s->cancel);
  if (s->timer) { g_source_remove(s->timer);s->timer=0; }
  if (s->ws) {
    g_signal_handlers_disconnect_by_data(s->ws,this);
    if(soup_websocket_connection_get_state(s->ws)==SOUP_WEBSOCKET_STATE_OPEN)
      soup_websocket_connection_close(s->ws,SOUP_WEBSOCKET_CLOSE_NORMAL,nullptr);
    g_clear_object(&s->ws);
  }
  if (s->soup) { soup_session_abort(s->soup);g_clear_object(&s->soup); }
  for (auto& [_,peer] : s->peers) if (peer->worker) peer->worker->stop();
  s->peers.clear();s->outgoing_bytes=0;
  ready={};message={};left={};status={};failed={};
}
}
