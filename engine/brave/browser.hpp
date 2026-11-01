/* SPDX-License-Identifier: GPL-3.0-or-later */
#pragma once
#include <gtk/gtk.h>
#include <json-glib/json-glib.h>
#include <functional>
#include <memory>
#include <string>
namespace GnosisBrave {
bool enabled();
void configure_debug_adapters(const std::string&);
bool adapter_enabled(const char*);
bool adapter_overridden(const char*);
std::string disabled_adapters();
bool composited();
void initialize(const std::string& profile);
void set_automation_mode(bool);
void shutdown();
void update_preferences(const std::string& json);
void set_user_content(const std::string& css, const std::string& script);
void browser_command(bool private_profile, const std::string& method, const std::string& params,
                     std::function<void(JsonObject*, const std::string&)> reply);
void clear_website_data(bool private_profile, const std::string& types,
                        std::function<void(JsonObject*, const std::string&)> reply);
// Normal profile only. Returns false until its trusted worker is ready.
void initialize_extensions(std::function<void()> changed);


std::string quote(const std::string&);
using Reply = std::function<void(JsonObject*, const std::string&)>;
bool extensions_load(const std::string& path, Reply);
bool extensions_call(const std::string& method, const std::string& arguments, Reply);
using ResourceReply = std::function<void(GBytes*, const std::string&, const std::string&)>;
using DownloadProgress = std::function<void(guint64, guint64, const std::string&)>;
using DownloadBegin = std::function<DownloadProgress(const std::string&, const std::string&,
  const std::string&, std::function<void()>)>;
class Process;
class Page : public std::enable_shared_from_this<Page> {
public:
  static std::shared_ptr<Page> create(GtkWidget* content, bool ephemeral);
  ~Page();
  std::string uri = "about:blank", title, target, session;
  bool loading = false, can_back = false, can_forward = false;
  double progress = 0;
  std::function<void()> changed, closed, tls_changed;
  std::function<void(bool)> fullscreen_changed;
  std::function<void(double)> zoom_changed;
  std::function<void(const std::string&, const std::string&, GdkTexture*)> favicon_changed;
  std::function<void(int, int)> popup_size_changed;
  std::function<void(const std::string&)> failed;
  std::function<void(const std::string&)> navigation_started;
  std::function<bool(const std::string&)> open_uri;
  std::function<void(int)> load_finished;
  std::function<void(const std::string&, JsonObject*)> event;
  std::function<void(const std::string&, bool, ResourceReply)> internal_resource;
  std::function<void(const std::string&, const std::string&, const std::string&)> page_message;
  DownloadBegin download_begin;
  std::string canonical_uri(const std::string&) const;
  bool is_internal_page(const std::string&) const;
  bool send_page_message(const std::string&, const std::string&, const std::string&);
  void start();
  void set_extension_popup(bool, int owner_tab_id = -1);
  void navigate(const std::string&);
  void command(const std::string&, const std::string& = "{}", Reply = {});
  // Route a trusted engine command through the session that owns this frame.
  // Runtime commands still need an explicit context for non-root local frames.
  void command_for_frame(const std::string&, const std::string&, const std::string&, Reply = {});
  void back(); void forward(); void reload(bool bypass = false); void stop();
  void find(const std::string&, bool forward, bool sensitive, bool match, Reply);
  void stop_find();
  void request_close(std::function<void(bool, bool, const std::string&)>);
  void cancel_close();
  bool close_is_approved() const;
  void focus(); void set_visible(bool); void close();
  void send_key(GdkEvent*);
  bool handle_vim_key(guint key, GdkModifierType modifiers);
  bool edit(const std::string&);
  void set_zoom(double);
  GtkWidget* create_translation_controls();
  void translation_request(const std::string& operation, const std::string& language, int sequence, Reply);
  GtkWidget* widget() const;
  int native_tab_id() const;
  void show_password_manager();
  void show_devtools();
  void close_devtools();
  bool devtools_open() const;
  void keepassxc_request(const std::string&, const std::string&, Reply);
  bool is_closed() const;
  bool tls_info(GTlsCertificate**, GTlsCertificateFlags*) const;
  bool tls_secure() const;
  void update_preferences();
  void update_user_content();
  void print_pdf(const std::string& path, GtkPageSetup*, GCancellable*,
                 std::function<void(const std::string&)>);
  void snapshot(std::function<void(GdkTexture*, const std::string&)>, bool full_document = false);
private:
  friend class Process;
  struct State;
  std::unique_ptr<State> state_;
  explicit Page(GtkWidget*, bool);
  void attach(); void handle(const std::string&, JsonObject*, bool frame_event = false); void layout();
  void adopt_surface(GtkWidget*); void surface_ready();
  void update_portal_parent();
  void history(int); void refresh_history();
  void internal_request(JsonObject*);
  void show_context_menu(JsonObject*, int context);
  void dismiss_context_menu();
  void show_script_dialog(JsonObject*);
  void show_close_failure(const std::string&);
  void dismiss_script_dialog(bool cancel);
  void show_install_dialog(JsonObject*);
  void dismiss_install_dialog(bool cancel);
  void dismiss_http_retry();
  void offer_http_retry(const std::string&);
  void theme_document(const std::string&, const std::string&);
  void update_web_theme();
  void update_vim(int context);
  void set_vim_context(int context);
  void vim_key(const std::string& command);
  void show_vim_commands();
  void dismiss_vim_commands();
  void execute_vim_command(const std::string& command);
  void request_favicon();
  void apply_zoom();
  void receive_zoom(double);
  void dismiss_password_manager();
  void show_authenticator(JsonObject*);
  void dismiss_authenticator(bool cancel);
  void show_passkey_provider(JsonObject*);
  void dismiss_passkey_provider();
  void authenticator_action(const std::string&, int, guint64);
  void poll_password_manager();
  void update_password_manager(JsonObject*, const std::string&);
  void password_manager_operation(std::string, std::string = "null");
  void dismiss_translation();
  void poll_translation();
  void update_translation(JsonObject*, const std::string&);
};
}
