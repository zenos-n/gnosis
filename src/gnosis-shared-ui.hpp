/* SPDX-License-Identifier: GPL-3.0-or-later */
#pragma once

#include <functional>
#include <memory>
#include <string>

#include <gtk/gtk.h>

typedef struct _EphyEngineView EphyEngineView;
typedef struct _EphyWindow EphyWindow;

namespace GnosisShared {

class Interface {
 public:
  static std::shared_ptr<Interface> create(GtkApplication *application);
  ~Interface();

  Interface(const Interface &) = delete;
  Interface &operator=(const Interface &) = delete;

  std::function<void(const std::string &name, const std::string &server)> host_requested;
  std::function<void(const std::string &invitation, const std::string &name)> join_requested;
  std::function<void()> stop_requested;
  std::function<void(const std::string &id)> approved;
  std::function<void(const std::string &id)> rejected;
  std::function<void(const std::string &id)> removed;
  std::function<void(const std::string &id, bool enabled)> control_changed;
  /* A factor of 0 requests fit-to-window; positive values are local zoom. */
  std::function<void(double factor)> zoom_changed;
  /* Width and height are the guest view's current logical-pixel allocation. */
  std::function<void(int width, int height)> resize_requested;

  void present(GtkWindow *parent);
  void active(bool host,
              const std::string &title,
              const std::string &invitation);
  void status(const std::string &text);
  void attach_host(EphyWindow *window);
  EphyEngineView *create_guest();
  void participant(const std::string &id,
                   const std::string &name,
                   bool pending,
                   bool control);
  void remove(const std::string &id);
  void ask_resize(const std::string &name,
                  int width,
                  int height,
                  std::function<void(bool)> reply);
  void clear();

 private:
  class Impl;
  explicit Interface(GtkApplication *application);
  std::unique_ptr<Impl> impl_;
};

}  // namespace GnosisShared
