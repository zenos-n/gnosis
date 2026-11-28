/* SPDX-License-Identifier: GPL-3.0-or-later */
#pragma once
#include <functional>
#include <memory>
#include <string>
#include <array>

namespace GnosisShare {
struct Invite {
  std::string server, room;
  std::array<unsigned char,32> key{};
  static Invite create(const std::string& server);
  static Invite parse(const std::string& link);
  std::string link() const;
};
class Transport : public std::enable_shared_from_this<Transport> {
public:
  // Call on the owning GLib main thread; callbacks return there. send returns
  // false under backpressure without consuming a sequence. close is idempotent.
  using Message = std::function<void(const std::string&,const std::string&)>;
  static std::shared_ptr<Transport> create(Invite, bool host);
  ~Transport();
  std::function<void(const std::string&,const std::string&)> ready;
  Message message;
  std::function<void(const std::string&)> left, status, failed;
  void connect();
  void allow(const std::string&);
  void kick(const std::string&);
  bool send(const std::string&,const std::string& json);
  void close();
  const Invite& invitation() const;
private:
  struct State;
  std::unique_ptr<State> s;
  Transport(Invite,bool);
  bool websocket(const std::string&);
  bool packet(const std::string&,const std::string&,bool signal);
  void receive(const std::string&,const std::string&);
  void peer(const std::string&,bool offer);
  void acknowledge(const std::string&);
  void tick();
  void drop(const std::string&);
  void fail(const std::string&);
};
}
