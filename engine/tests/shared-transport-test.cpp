/* SPDX-License-Identifier: GPL-3.0-or-later */
#include <array>
#include <functional>
#include <memory>
#include <string>
// Exercise authenticated envelopes and queue boundaries without a production test API.
#define private public
#include "../../src/gnosis-share-transport.hpp"
#undef private
#include "../../src/gnosis-share-transport.cpp"
#include <cassert>
#include <chrono>
#include <iostream>
#include <vector>

using namespace GnosisShare;
namespace {
const std::string alice(32,'a'),bob(32,'b'),charlie(32,'c');
const auto main_thread = std::this_thread::get_id();
void pump() { while (g_main_context_pending(nullptr)) g_main_context_iteration(nullptr,false); }
void wait(const std::function<bool()>& predicate,int milliseconds=10000) {
  const auto end = g_get_monotonic_time()+milliseconds*1000;
  while (!predicate()) {
    assert(g_get_monotonic_time()<end);
    pump();std::this_thread::sleep_for(std::chrono::milliseconds(1));
  }
  pump();
}
std::string seal(const Invite& invite,const std::string& from,const std::string& to,int sequence,const std::string& data,bool signal=false) {
  auto plain = "{\"from\":\""+from+"\",\"to\":\""+to+"\",\"seq\":"+std::to_string(sequence)+
    ",\"signal\":"+(signal?"true":"false")+",\"data\":"+data+"}";
  std::string box(40+plain.size(),0);
  auto* bytes = reinterpret_cast<unsigned char*>(box.data());randombytes_buf(bytes,24);
  crypto_secretbox_easy(bytes+24,reinterpret_cast<const unsigned char*>(plain.data()),plain.size(),bytes,invite.key.data());
  return box;
}
void envelopes() {
  auto invite = Invite::create("ws://127.0.0.1:1");
  assert(Invite::parse(invite.link()).link()==invite.link());
  for (const auto& invalid : {"ws://example.test", "wss://user:secret@example.test", "wss://example.test/#x"}) {
    bool rejected=false;try { Invite::create(invalid); } catch (const std::exception&) { rejected=true; }assert(rejected);
  }
  auto receiver = Transport::create(invite,false);receiver->s->id=bob;receiver->s->host_id=alice;
  auto peer = receiver->s->peers[alice]=std::make_shared<Transport::State::Peer>();peer->approved=true;
  std::vector<std::string> delivered;
  receiver->message=[&](const auto&,const auto& json) { delivered.push_back(json); };
  auto first = seal(invite,alice,bob,1,"{\"n\":1}");
  auto second = seal(invite,alice,bob,2,"{\"n\":2}");
  receiver->receive(alice,second);assert(delivered.empty());
  receiver->receive(alice,first);assert((delivered==std::vector<std::string>{"{\"n\":1}","{\"n\":2}"}));
  receiver->receive(alice,first);receiver->receive(alice,second);assert(delivered.size()==2);
  receiver->receive(alice,seal(invite,alice,charlie,3,"{\"n\":3}"));assert(delivered.size()==2);
  receiver->receive(charlie,seal(invite,alice,bob,3,"{\"n\":3}"));assert(delivered.size()==2);
  auto bad = seal(invite,alice,bob,3,"{\"n\":3}");bad.back()^=1;receiver->receive(alice,bad);assert(delivered.size()==2);
  auto wrong = invite;wrong.key[0]^=1;
  receiver->receive(alice,seal(wrong,alice,bob,3,"{\"n\":3}"));assert(delivered.size()==2);
  receiver->receive(alice,seal(invite,alice,bob,50000,"{\"n\":3}"));assert(delivered.size()==2);
  receiver->receive(alice,seal(invite,alice,bob,3,"{\"n\":3}"));assert(delivered.size()==3);
  // An application sequence gap must not block the independent ICE/SDP lane.
  receiver->receive(alice,seal(invite,alice,bob,5,"{\"n\":5}"));
  receiver->receive(alice,seal(invite,alice,bob,1,"{\"kind\":\"ice\"}",true));
  assert(peer->lanes[1].next==2);assert(delivered.size()==3);
  receiver->receive(alice,seal(invite,alice,bob,4,"{\"n\":4}"));assert(delivered.size()==5);
  // Reentrant stop from a delivered callback cannot use a destroyed peer/function.
  receiver->message=[&](const auto&,const auto&) { receiver->close(); };
  receiver->receive(alice,seal(invite,alice,bob,6,"{\"n\":6}"));
  assert(receiver->s->closed);
  auto bounded=Transport::create(invite,false);bounded->s->id=bob;bounded->s->host_id=alice;
  auto slot=bounded->s->peers[alice]=std::make_shared<Transport::State::Peer>();slot->approved=true;
  const std::string large="{\"large\":\""+std::string(3*1024*1024,'x')+"\"}";
  bounded->receive(alice,seal(invite,alice,bob,2,large));
  bounded->receive(alice,seal(invite,alice,bob,3,large));
  const auto bytes=slot->waiting_bytes;
  bounded->receive(alice,seal(invite,alice,bob,4,large));
  assert(slot->waiting_bytes==bytes);assert(slot->waiting_bytes<8*1024*1024);
  bounded->close();
}

struct Pair {
  std::shared_ptr<Transport> host,guest;
  std::string host_id,guest_id,error;
  bool hello=false,allowed=false,host_p2p=false,guest_p2p=false;
  std::vector<int> received;
  explicit Pair(const std::string& url) {
    auto invite=Invite::create(url);host=Transport::create(invite,true);guest=Transport::create(invite,false);
    host->failed=guest->failed=[&](const auto& why) { error=why; };
    host->ready=[&](const auto& id,const auto&) { host_id=id; };
    host->message=[&](const auto& from,const auto& json) { assert(std::this_thread::get_id()==main_thread);assert(from==guest_id);if (json=="{\"kind\":\"hello\"}") hello=true; };
    host->status=[&](const auto& status) { assert(std::this_thread::get_id()==main_thread);if(status=="Connected peer-to-peer")host_p2p=true; };
    guest->ready=[&](const auto& id,const auto& owner) { guest_id=id;assert(owner==host_id);assert(guest->send(owner,"{\"kind\":\"hello\"}")); };
    guest->status=[&](const auto& status) { assert(std::this_thread::get_id()==main_thread);if(status=="Connected through relay")allowed=true;if(status=="Connected peer-to-peer")guest_p2p=true; };
    guest->message=[&](const auto&,const auto& json) {
      assert(std::this_thread::get_id()==main_thread);
      g_autoptr(JsonParser) parser=json_parser_new();assert(json_parser_load_from_data(parser,json.data(),json.size(),nullptr));
      auto* object=json_node_get_object(json_parser_get_root(parser));
      if(json_object_has_member(object,"n"))received.push_back(json_object_get_int_member(object,"n"));
    };
    host->connect();host->connect();wait([&] { return !host_id.empty()||!error.empty(); });assert(error.empty());
    guest->connect();wait([&] { return hello||!error.empty(); });assert(error.empty());
    assert(!guest->send(host_id,"{\"kind\":\"before-approval\"}"));
  }
  void approve() {
    const auto start=g_get_monotonic_time();host->allow(guest_id);
    assert(g_get_monotonic_time()-start<400000);
    wait([&] { return allowed||!error.empty(); });assert(error.empty());
  }
  ~Pair() { if(host)host->close();if(guest)guest->close();pump(); }
};
void network(const std::string& url,bool p2p) {
  Pair pair(url);pair.approve();
  assert(!pair.host->send(pair.guest_id,"not JSON"));
  assert(!pair.host->send(pair.guest_id,"{\"large\":\""+std::string(4*1024*1024,'x')+"\"}"));
  // Saturating accepted sends must apply backpressure without consuming missing sequence numbers.
  size_t sent=0;
  while(pair.host->send(pair.guest_id,"{\"n\":"+std::to_string(sent)+"}")) { sent++;assert(sent<=128); }
  assert(sent>0);assert(sent<=128);
  assert(pair.host->s->outgoing_bytes<16*1024*1024);
  wait([&] { return pair.received.size()==sent||!pair.error.empty(); });assert(pair.error.empty());
  if(p2p) { wait([&] { return (pair.host_p2p&&pair.guest_p2p)||!pair.error.empty(); },15000);assert(pair.error.empty()); }
  const auto total=600u;
  while(sent<total) {
    assert(pair.error.empty());
    if(pair.host->send(pair.guest_id,"{\"n\":"+std::to_string(sent)+"}"))sent++;
    else { pump();std::this_thread::sleep_for(std::chrono::milliseconds(1)); }
  }
  wait([&] { return pair.received.size()==total||!pair.error.empty(); });assert(pair.error.empty());
  for(size_t i=0;i<total;i++)assert(pair.received[i]==static_cast<int>(i));
  assert(pair.host->send(pair.guest_id,"{\"n\":600,\"large\":\""+std::string(1024*1024,'x')+"\"}"));
  wait([&] { return pair.received.size()==601||!pair.error.empty(); });assert(pair.error.empty());
  if(p2p) {
    // A direct packet can still be in flight when its channel is closed. Relay retransmit fills the gap.
    assert(pair.host->send(pair.guest_id,"{\"n\":601}"));
    auto worker=pair.host->s->peers.at(pair.guest_id)->worker;
    assert(worker->submit([](Worker& value) { if(value.dc)value.dc->close(); }));
    assert(pair.host->send(pair.guest_id,"{\"n\":602}"));
    wait([&] { return pair.received.size()==603||!pair.error.empty(); });assert(pair.error.empty());
    assert(pair.received[601]==601&&pair.received[602]==602);
  }
  // Model a packet lost during direct-channel failure, then exercise real encrypted relay recovery.
  const auto count=pair.received.size();
  auto peer=pair.host->s->peers.at(pair.guest_id);auto& lane=peer->lanes[0];
  auto box=seal(pair.host->invitation(),pair.host_id,pair.guest_id,lane.sent+1,"{\"n\":"+std::to_string(count)+"}");
  lane.outgoing.emplace(++lane.sent,Transport::State::Outgoing{box,g_get_monotonic_time()-1600000,false,false});
  peer->outgoing_bytes+=box.size();pair.host->s->outgoing_bytes+=box.size();
  pair.host->tick();
  wait([&] { return pair.received.size()==count+1||!pair.error.empty(); });assert(pair.error.empty());
  assert(pair.received.back()==static_cast<int>(count));
  bool left=false;
  pair.host->left=[&](const auto& id) { assert(id==pair.guest_id);left=true; };
  pair.guest->close();wait([&] { return left||!pair.error.empty(); });assert(pair.error.empty());
}
void wrong_key(const std::string& url) {
  auto invite=Invite::create(url);auto wrong=invite;wrong.key[0]^=1;
  auto host=Transport::create(invite,true),guest=Transport::create(wrong,false);
  std::string host_id,guest_id,error;int delivered=0;
  host->ready=[&](const auto& id,const auto&) { host_id=id; };
  host->failed=guest->failed=[&](const auto& why) { error=why; };
  host->message=[&](const auto&,const auto&) { delivered++; };
  guest->ready=[&](const auto& id,const auto& owner) { guest_id=id;assert(guest->send(owner,"{\"kind\":\"hello\"}")); };
  host->connect();wait([&] { return !host_id.empty()||!error.empty(); });assert(error.empty());
  guest->connect();wait([&] { return !guest_id.empty()||!error.empty(); });assert(error.empty());
  const auto until=g_get_monotonic_time()+200000;
  while(g_get_monotonic_time()<until) { pump();std::this_thread::sleep_for(std::chrono::milliseconds(1)); }
  assert(delivered==0);assert(host->s->peers.empty());
  host->close();guest->close();
}
void cancellation(const std::string& url) {
  int callbacks=0;
  for(int i=0;i<20;i++) {
    auto transport=Transport::create(Invite::create(url),true);
    transport->ready=[&](const auto&,const auto&) { callbacks++; };
    transport->failed=[&](const auto&) { callbacks++; };
    transport->connect();transport->close();transport.reset();
  }
  const auto until=g_get_monotonic_time()+300000;
  while(g_get_monotonic_time()<until) { pump();std::this_thread::sleep_for(std::chrono::milliseconds(1)); }
  assert(callbacks==0);
  Pair pending(url);
  const auto started=g_get_monotonic_time();
  pending.host->allow(pending.guest_id);
  pending.host->close();pending.guest->close();
  assert(g_get_monotonic_time()-started<400000);
  std::weak_ptr<Transport> weak=pending.host;pending.host.reset();
  wait([&] { return weak.expired(); });
}
}
int main(int argc,char** argv) {
  assert(argc==3);
  const bool p2p=std::string(argv[2])=="p2p";
  if(p2p)g_unsetenv("GNOSIS_SHARE_DISABLE_P2P");else g_setenv("GNOSIS_SHARE_DISABLE_P2P","1",true);
  g_setenv("GNOSIS_SHARE_STUN_SERVER","",true);
  envelopes();network(argv[1],p2p);wrong_key(argv[1]);cancellation(argv[1]);
  std::cout<<"PASS "<<argv[2]<<": authentication, recipient binding, ordering/backpressure, wrong key, stop/lifecycle\n";
}
