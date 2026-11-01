/* SPDX-License-Identifier: GPL-3.0-or-later
 * Native-window embedding extracted from the visibly verified Gnosis Brave PoC.
 */
#include "browser.hpp"
#include "compositor.h"
#include "profile.h"
#include "private-profile.hpp"
#include "bundled-extension.hpp"
#include "../loopback.h"
#include "../web-app-config.h"
#include "../select/extensions.hpp"
#include <gdk/x11/gdkx.h>
#include <gdk/wayland/gdkwayland.h>
#include <glib-unix.h>
#include <libsoup/soup.h>
#include <X11/Xlib.h>
#include <X11/Xatom.h>
#include <X11/Xutil.h>
#include <X11/keysym.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/socket.h>
#include <algorithm>
#include <cerrno>
#include <csignal>
#include <map>
#include <deque>
#include <set>
#include <vector>
#include <cstring>
#include <cmath>
#include <utility>
#include <filesystem>
#include <glib/gi18n.h>
#include <adwaita.h>
#include <qrencode.h>
namespace GnosisBrave {
namespace {
std::string profile_root;
std::string web_color_scheme() {
  return "{\"features\":[{\"name\":\"prefers-color-scheme\",\"value\":" +
    quote(adw_style_manager_get_dark(adw_style_manager_get_default()) ? "dark" : "light") + "}]}";
}
bool automation_mode = false;
bool vim_mode = false;
const std::string popup_layout = R"CSS(
html { inline-size: max-content !important; block-size: max-content !important;
  min-inline-size: 0 !important; min-block-size: 0 !important;
  max-inline-size: 800px !important; overflow: auto !important; }
body { min-block-size: 0 !important; }
)CSS";
std::function<void()> extensions_changed;
// Fixed by the public key in the packaged native-integration manifest.
constexpr const char* native_integration_id = "lplpdmfakaadhenboelgbkbdngpldloj";
std::map<bool, std::shared_ptr<Process>> processes;
std::vector<std::shared_ptr<Process>> retiring_private_processes;
std::string native_preferences = "{}", user_stylesheet, user_javascript;
const char* text(JsonObject* o, const char* key) { return o ? json_object_get_string_member_with_default(o, key, "") : ""; }
JsonObject* object(JsonObject* o, const char* key) { return o && json_object_has_member(o, key) ? json_object_get_object_member(o, key) : nullptr; }
std::string document_adapter_source() {
  std::string result;
  if (!adapter_enabled("document-metadata")) return result;
  for (const char* name : {"document-metadata.js", "vim-field.js", "vim-mode.js"}) {
    if (strcmp(name,"document-metadata.js") && !adapter_enabled("vim")) continue;
    g_autofree char* path = g_build_filename(GNOSIS_NATIVE_INTEGRATION_DIR, name, nullptr);
    g_autofree char* source = nullptr;
    if (g_file_get_contents(path, &source, nullptr, nullptr)) result += std::string(source) + "\n";
    else g_warning("The native document adapter could not be loaded: %s", name);
  }
  return result;
}
int ignore_x_error(Display*, XErrorEvent*) { return 0; }
}
bool enabled() { return g_strcmp0(g_getenv("GNOSIS_ENGINE"), "brave") == 0; }
bool composited() { return g_strcmp0(g_getenv("GNOSIS_SURFACE"), "x11") != 0; }
std::string quote(const std::string& value) {
  auto* node = json_node_new(JSON_NODE_VALUE); json_node_set_string(node, value.c_str());
  char* json = json_to_string(node, false); std::string result(json); g_free(json); json_node_free(node); return result;
}
namespace {
bool internal_document(const std::string& value) { return value.starts_with("ephy-about:"); }
bool served_document(const std::string& value) {
  return internal_document(value) || value.starts_with("ephy-reader:");
}
std::string transport_uri(const std::string& origin, const std::string& value) {
  for (const char* scheme : {"ephy-about", "ephy-resource", "ephy-reader"}) {
    std::string prefix = std::string(scheme) + ':';
    if (value.starts_with(prefix)) return origin + '/' + scheme + '/' + value.substr(prefix.size());
  }
  return value;
}
std::string rewrite_resources(std::string value, const std::string& origin) {
  for (const char* scheme : {"ephy-about", "ephy-resource", "ephy-reader"}) {
    std::string from = std::string(scheme) + ':', to = origin + '/' + scheme + '/';
    for (size_t pos = 0; (pos = value.find(from, pos)) != std::string::npos; pos += to.size()) value.replace(pos, from.size(), to);
  }
  return value;
}
}
static guint64 translation_dialog_sequence;
struct TranslationUI {
  guint64 identity = ++translation_dialog_sequence;
  GtkWidget* box = nullptr;
  GtkDropDown* language = nullptr;
  GtkLabel* status = nullptr;
  GtkSpinner* spinner = nullptr;
  GtkWidget *translate = nullptr, *original = nullptr;
  guint poll = 0;
  int sequence = -1;
  guint64 generation = 0;
  gint64 started = 0;
  bool waiting = false;
};
struct PasswordUI {
  AdwDialog* dialog = nullptr;
  AdwActionRow* status = nullptr;
  AdwPreferencesGroup* entries = nullptr;
  GtkWidget *connect = nullptr, *unlock = nullptr, *lock = nullptr, *list = nullptr, *totp = nullptr, *reconnect = nullptr, *redetect = nullptr;
  AdwComboRow* manager = nullptr;
  AdwSwitchRow *passkeys = nullptr, *auto_unlock = nullptr;
  GtkSpinner* spinner = nullptr;
  std::vector<GtkWidget*> rows;
  gulong closed = 0;
  guint refresh = 0;
  guint64 identity = ++translation_dialog_sequence;
  bool busy = false, updating = false;
};
struct AuthenticatorUI {
  AdwDialog* dialog = nullptr;
  GtkWidget* content = nullptr;
  GtkWidget* toolbar = nullptr;
  GtkWidget* header = nullptr;
  GtkWidget* scroll = nullptr;
  GtkWidget* footer = nullptr;
  gint64 id = 0;
  guint64 revision = 0;
  guint enable_timer = 0;
  gulong closed = 0;
  std::vector<GtkWidget*> actions;
};
struct Page::State {
  GtkWidget* content;
  GtkEventController* keys = nullptr;
  GtkWidget* surface = nullptr;
  std::shared_ptr<Process> process;
  std::string token, bootstrap, requested = "about:blank", main_frame;
  std::string internal_origin, context_unique;
  std::string authorized_document, fulfilled_document, committed_document;
  std::map<std::string, std::string> internal_loaders;
  guint64 navigation_generation = 0;
  guint64 find_generation = 0;
  guint64 close_generation = 0, close_navigation = 0;
  gint64 close_request = 0;
  guint close_watch = 0;
  gint64 close_started = 0;
  bool close_approved = false, close_prompted = false;
  AdwDialog* close_dialog = nullptr;
  gulong close_dialog_response = 0;
  std::function<void(bool, bool, const std::string&)> close_callback;
  struct ThemeFrame { std::string loader, sheet; guint64 generation = 0; bool pending = false; };
  std::map<std::string, ThemeFrame> theme_frames;
  guint64 theme_generation = 0;
  std::string favicon_uri, favicon_document;
  guint64 favicon_generation = 0;
  bool theme_ready = false;
  gulong theme_signals[3] = {};
  int context_id = 0;
  bool protocol_ready = false;
  Display* display = nullptr;
  ::Window parent = 0, child = 0, container = 0;
  int window = -1, width = 0, height = 0;
  int native_tab = -1;
  bool extension_popup = false;
  int popup_width = 0, popup_height = 0;
  double zoom_requested = 1, zoom_applied = 1;
  bool zoom_pending = false, zoom_busy = false;
  guint64 zoom_revision = 0;
  int response_status = 0;
  GTlsCertificate* certificate = nullptr;
  GTlsCertificateFlags tls_errors = GTlsCertificateFlags(0);
  std::string security_state;
  gint64 withdrawn_until = 0;
  guint timer = 0;
  bool ephemeral, started = false, disposed = false, mapped = true, attached = false, native_destroyed = false;
  GtkWindow* devtools_window = nullptr;
  std::string devtools_target;
  GtkWidget* context_menu = nullptr;
  AdwDialog* script_dialog = nullptr;
  gulong script_dialog_response = 0;
  guint64 script_dialog_generation = 0;
  bool script_dialog_open = false;
  std::unique_ptr<TranslationUI> translation;
  std::unique_ptr<PasswordUI> passwords;
  std::unique_ptr<AuthenticatorUI> authenticator;
  GCancellable* passkey_provider = nullptr;
  std::string passkey_provider_id;
  AdwDialog* install_dialog = nullptr;
  AdwDialog* http_retry = nullptr;
  std::string document_request, document_request_url;
  gulong install_response = 0;
  gint64 install_id = 0;
  guint install_delay = 0;
  std::map<int, std::string> menu_contexts, menu_frames, menu_sessions, metadata_contexts, metadata_unique;
  int vim_context = 0;
  GtkWidget *vim_commands = nullptr, *vim_command_entry = nullptr, *vim_command_error = nullptr;
  std::string document_title_uri;
  std::string user_script_id, applied_user_script;
  guint64 user_content_generation = 0;
  std::vector<std::function<void()>> waiting;
  explicit State(GtkWidget* widget, bool private_mode) : content(widget), ephemeral(private_mode) {
    char* id = g_uuid_string_random(); token = id; g_free(id);
    internal_origin = "https://" + token + ".gnosis.invalid";
  }
  ~State() { g_clear_object(&certificate); }
};
class Process : public std::enable_shared_from_this<Process> {
public:
  std::string profile, window_class, base, product;
  GSubprocess* browser = nullptr;
  SoupServer* server = nullptr;
  GnosisCompositor* compositor = nullptr;
  int input = -1, output = -1, sequence = 0;
  guint watch = 0, native_watch = 0;
  int native_fd = -1;
  std::string portal_target;
  gint64 native_sequence = 0;
  struct NativeReply { Reply callback; guint timeout; };
  std::map<gint64, NativeReply> native_replies;
  bool closing = false, ready = false, launched = false, restarting = false;
  bool browser_waiting = false;
  unsigned profile_writers = 0;
  std::shared_ptr<PrivateProfile> private_storage;
  bool integration_ready = false, integration_connecting = false;
  bool debug_startup_pending = false;
  bool preferences_busy = false, preferences_again = false, shields_configured = false;
  std::string settings_target, settings_session;
  bool extensions_ready = false, private_profile = false;
  std::string integration_id;
  std::string integration_adapter;
  std::string integration_session, keepassxc_session, keepassxc_target;
  guint keepassxc_poll = 0;
  gint64 keepassxc_deadline = 0;
  bool keepassxc_ready = false;
  guint integration_poll = 0, extensions_poll = 0;
  gint64 extensions_deadline = 0;
  gint64 integration_deadline = 0;
  std::string buffer;
  std::map<int, Reply> replies;
  struct StyleCommand { std::string method, params; Reply reply; };
  struct StyleSession { std::deque<StyleCommand> pending; };
  std::map<std::string, StyleSession> style_sessions;
  std::map<std::string, std::weak_ptr<Page>> pages, targets, sessions;
  std::vector<std::weak_ptr<Page>> pending_opens;
  std::map<int,std::weak_ptr<Page>> zoom_tabs;
  std::map<std::string, std::weak_ptr<Page>> frame_owners;
  std::map<std::string, DownloadProgress> downloads;
  std::set<std::string> download_guids;
  std::string download_directory;
  struct ParkedPage { std::string target, session; ::Window child; GtkWidget* surface = nullptr; };
  std::vector<ParkedPage> parked;
#include "web-app-extensions.inc"
#include "keepassxc-pairing.inc"
  explicit Process(bool ephemeral, const std::string& reuse_profile = "") : private_profile(ephemeral) {
    read_webapp_extensions();
    configure_debug_adapters(native_preferences);
    debug_startup_pending = adapter_overridden("keepassxc") || adapter_overridden("ublock");
    if (composited()) {
      GError* failure = nullptr;
      compositor = gnosis_compositor_new(&failure);
      if (!compositor) {
        g_warning("Brave compositor: %s", failure->message); g_error_free(failure); return;
      }
    }
    if (!reuse_profile.empty()) profile = reuse_profile;
    else if (ephemeral) {
      private_storage = PrivateProfile::create();
      if (!private_storage) return;
      profile = private_storage->path();
    }
    else profile = profile_root + "/brave";
    GError* profile_error = nullptr;
    if (!gnosis_brave_profile_prepare(profile.c_str(), &profile_error)) {
      g_warning("Brave profile preparation: %s", profile_error->message); g_error_free(profile_error); return;
    }
    if (!ephemeral && webapp_keepassxc && adapter_enabled("keepassxc") && *GNOSIS_KEEPASSXC_DIR) {
      if (!gnosis_brave_profile_prepare_keepassxc(profile.c_str(), &profile_error)) {
        g_warning("KeePassXC integration: %s", profile_error->message); g_clear_error(&profile_error);
      }
    }
    std::string staging = profile + "/.downloads-XXXXXX";
    char* directory = g_mkdtemp(staging.data());
    if (!directory) { g_warning("Cannot create Brave's private download directory"); return; }
    download_directory = directory;
    char* id = g_uuid_string_random(); window_class = std::string("GnosisBrave-") + id; g_free(id);
    server = soup_server_new(nullptr, nullptr);
    soup_server_add_handler(server, nullptr, +[](SoupServer*, SoupServerMessage* message, const char* path, GHashTable*, gpointer data) {
      auto* self = static_cast<Process*>(data);
      const std::string key = path && g_str_has_prefix(path, "/bootstrap/") ? path + 11 : "";
      auto found = self->pages.find(key);
      if (found == self->pages.end() || found->second.expired()) { soup_server_message_set_status(message, SOUP_STATUS_NOT_FOUND, nullptr); return; }
      std::string html = "<!doctype html><title>GnosisTab-" + key + "</title><style>:root{color-scheme:dark light}</style>";
      soup_server_message_set_status(message, SOUP_STATUS_OK, nullptr);
      soup_message_headers_replace(soup_server_message_get_response_headers(message), "Cache-Control", "no-store");
      soup_message_headers_replace(soup_server_message_get_response_headers(message), "Referrer-Policy", "no-referrer");
      soup_server_message_set_response(message, "text/html", SOUP_MEMORY_COPY, html.data(), html.size());
    }, this, nullptr);
    GError* error = nullptr;
    if (!soup_server_listen_local(server, 0, SoupServerListenOptions(SOUP_SERVER_LISTEN_IPV4_ONLY), &error)) {
      g_warning("Brave bootstrap server: %s", error->message); g_error_free(error); return;
    }
    GSList* uris = soup_server_get_uris(server);
    if (uris) { char* value = g_uri_to_string(static_cast<GUri*>(uris->data)); base = value; g_free(value); }
    g_slist_free_full(uris, reinterpret_cast<GDestroyNotify>(g_uri_unref));
  }
  ~Process() {
    stop_keepassxc_pairing();
    if (keepassxc_poll) g_source_remove(keepassxc_poll);
    stop();
    for (auto& page : parked) if (page.surface) g_object_unref(page.surface);
    parked.clear();
    g_clear_object(&server); g_clear_object(&compositor);
    if (!download_directory.empty()) {
      GDir* directory = g_dir_open(download_directory.c_str(), 0, nullptr);
      if (directory) {
        while (const char* name = g_dir_read_name(directory)) {
          g_autofree char* path = g_build_filename(download_directory.c_str(), name, nullptr);
          unlink(path);
        }
        g_dir_close(directory);
      }
      rmdir(download_directory.c_str());
    }
  }
  void command(const std::string& method, const std::string& params, const std::string& session, Reply reply = {}) {
    if (closing || output < 0) { if (reply) reply(nullptr, "The Brave browser is closed"); return; }
    if ((!adapter_enabled("page-style") && (method.starts_with("DOM.") || method.starts_with("CSS."))) ||
        (!adapter_enabled("network-events") && method == "Network.enable") ||
        (!adapter_enabled("extension-events") && method == "Runtime.addBinding" &&
         (params.find("__gnosisZoomChanged") != std::string::npos || params.find("__gnosisExtensionsChanged") != std::string::npos ||
          params.find("__gnosisNavigationTargetCreated") != std::string::npos))) {
      if (reply) reply(nullptr, "Adapter disabled in Debugging settings");
      return;
    }
    int id = ++sequence;
    std::string json = "{\"id\":" + std::to_string(id) + ",\"method\":" + quote(method) + ",\"params\":" + params;
    if (!session.empty()) json += ",\"sessionId\":" + quote(session);
    json += '}'; json.push_back('\0');
    if (reply) replies.emplace(id, std::move(reply));
    for (size_t offset = 0; offset < json.size();) {
      ssize_t count = write(output, json.data() + offset, json.size() - offset);
      if (count < 0 && errno == EINTR) continue;
      if (count <= 0) { fail("Brave's private protocol pipe closed"); return; }
      offset += count;
    }
  }
  // Inspector-created styles survive CSS.disable. Keep its mutation observers
  // enabled only while this session's queued theme writes are in flight.
  void next_style_command(const std::string& session, const std::string& error = {}) {
    auto found = style_sessions.find(session);
    if (found == style_sessions.end()) return;
    if (found->second.pending.empty()) {
      command("CSS.disable", "{}", session);
      command("DOM.disable", "{}", session);
      style_sessions.erase(found);
      return;
    }
    auto item = std::move(found->second.pending.front());
    found->second.pending.pop_front();
    if (!error.empty()) {
      if (item.reply) item.reply(nullptr, error);
      next_style_command(session, error);
      return;
    }
    auto done = [weak = weak_from_this(), session, reply = std::move(item.reply)](JsonObject* result, const std::string& failure) {
      if (reply) reply(result, failure);
      if (auto process = weak.lock()) process->next_style_command(session);
    };
    command(item.method, item.params, session, std::move(done));
  }
  void style_command(const std::string& method, const std::string& params, const std::string& session, Reply reply) {
    if (!adapter_enabled("page-style") || closing || output < 0) {
      if (reply) reply(nullptr, "Page styling is unavailable");
      return;
    }
    const bool starting = !style_sessions.contains(session);
    style_sessions[session].pending.push_back({method, params, std::move(reply)});
    if (!starting) return;
    command("DOM.enable", "{}", session);
    command("CSS.enable", "{}", session, [weak = weak_from_this(), session](JsonObject*, const std::string& error) {
      if (auto process = weak.lock()) process->next_style_command(session, error);
    });
  }
  void fail(const std::string& error) {
    auto pending = std::move(replies); replies.clear();
    for (auto& [id, reply] : pending) reply(nullptr, error);
  }
  void fail_downloads() {
    auto pending = std::move(downloads); downloads.clear();
    for (auto& [guid, progress] : pending) progress(0, 0, "failed");
  }
  void restart() {
    if (closing || restarting || !composited()) return;
    restarting = true;
    g_message("Brave requested a shell-managed engine restart");
    command("Browser.close", "{}", "");
  }
  void disconnected() {
    if (closing) return;
    closing = true;
    if (watch) { g_source_remove(watch); watch = 0; }
    if (integration_poll) { g_source_remove(integration_poll); integration_poll = 0; }
    if (extensions_poll) { g_source_remove(extensions_poll); extensions_poll = 0; }
    forget_frames();
    close_native_ui();
    fail_downloads();
    fail("The Brave browser process exited");
    if (input >= 0) ::close(input);
    if (output >= 0) ::close(output);
    input = output = -1;
    g_clear_object(&browser);
    if (server) soup_server_disconnect(server);
    std::vector<std::shared_ptr<Page>> affected;
    for (auto& [token, weak] : pages) if (auto page = weak.lock()) affected.push_back(page);
    if (restarting && !affected.empty()) {
      // The launcher has exited before reusing its profile. Retain GTK tabs and
      // callbacks, replacing just the dead engine connections and surfaces.
      auto keep_alive = shared_from_this();
      const bool ephemeral = affected.front()->state_->ephemeral;
      auto replacement = std::make_shared<Process>(ephemeral, profile);
      replacement->private_storage = std::move(private_storage);
      processes[ephemeral] = replacement;
      for (auto& page : affected) {
        auto& s = *page->state_;
        page->dismiss_context_menu(); page->dismiss_script_dialog(false); page->dismiss_install_dialog(false);
        if (s.surface) {
          g_signal_handlers_disconnect_by_data(s.surface, page.get());
          gtk_box_remove(GTK_BOX(s.content), s.surface); s.surface = nullptr;
        }
        s.requested = page->uri;
        s.attached = s.protocol_ready = s.native_destroyed = false;
        s.window = s.native_tab = -1; s.child = 0;
        page->set_vim_context(0); s.main_frame.clear(); s.context_id = 0; s.context_unique.clear(); s.menu_contexts.clear(); s.menu_frames.clear(); s.menu_sessions.clear(); s.metadata_contexts.clear(); s.metadata_unique.clear();
        s.zoom_busy = false; s.zoom_pending = true; ++s.zoom_revision;
        s.process = replacement;
        page->target.clear(); page->session.clear();
        page->loading = true; page->progress = 0;
        if (page->changed) page->changed();
      }
      // Open the visible tab first. Existing startup serialization queues the
      // others until the new browser owns its profile and private pipe.
      std::stable_sort(affected.begin(), affected.end(), [](const auto& a, const auto& b) {
        return gtk_widget_get_mapped(a->widget()) && !gtk_widget_get_mapped(b->widget());
      });
      for (auto& page : affected) replacement->open(page);
      return;
    }
    restarting = false;
    for (auto& page : affected) {
      auto failed = page->failed;
      page->close();
      if (failed) failed("The browser process stopped. Reload this tab to reconnect.");
    }
  }
  void open(const std::shared_ptr<Page>& page) {
    if (base.empty()) {
      closing = true;
      auto failed = page->failed; page->close();
      if (failed) failed("Could not initialize the Brave content backend. Reload to retry.");
      return;
    }
    pages[page->state_->token] = page;
    // Restored tabs arrive together. Until the first process owns its profile
    // and protocol pipe, another --app launcher can win Chromium's singleton
    // race and leave us connected to the process that exits instead.
    if (launched && !ready) {
      pending_opens.push_back(page);
      return;
    }
    g_message("Brave tab requested: token=%s reuse=%s", page->state_->token.c_str(), parked.empty() ? "no" : "yes");
    if (!parked.empty() && !page->state_->extension_popup) {
      // Keep one hidden app window alive when its GTK tab closes. Otherwise
      // Chromium exits after the last app window and a concurrent --app launch
      // can become an uncontrolled process with no inherited protocol pipe.
      auto reusable = std::move(parked.back()); parked.pop_back();
      page->target = reusable.target; page->session = reusable.session;
      page->state_->child = reusable.child;
      if (reusable.surface) {
        page->adopt_surface(reusable.surface); g_object_unref(reusable.surface);
      }
      targets[page->target] = page; sessions[page->session] = page;
      page->command("Page.resetNavigationHistory");
      page->attach();
      return;
    }
    page->state_->bootstrap = base + "bootstrap/" + page->state_->token;
    if (composited()) page->adopt_surface(gnosis_compositor_view_new_for_server(compositor, page->state_->token.c_str()));
    const char* executable = g_getenv(!adapter_enabled("scrollbars") ?
      "GNOSIS_BRAVE_STOCK_SCROLLBARS_EXECUTABLE" : "GNOSIS_BRAVE_EXECUTABLE");
    if (!executable) executable = g_getenv("GNOSIS_BRAVE_EXECUTABLE");
    std::vector<std::string> args{executable ? executable : "brave",
      "--user-data-dir=" + profile, "--app=" + page->state_->bootstrap, "--class=" + window_class,
      composited() ? "--ozone-platform=wayland" : "--ozone-platform=x11", "--no-first-run", "--no-default-browser-check", "--disable-background-mode",
      // Brave disables the real File System Access pickers unless its base
      // feature is explicitly enabled. Keep the Nix launcher's acceleration
      // switches when supplying the combined feature list.
      // CustomScrollbar's class-local overlay patch also needs the native
      // overlay theme so visibility, hit testing and fade timers agree.
      std::string("--enable-features=FileSystemAccessAPI,BraveWebBluetoothAPI,AcceleratedVideoDecodeLinuxGL,AcceleratedVideoEncoder,OverlayScrollbar:mode/overlay") + (composited() ? ",WaylandExternalBeginFrameSource" : ""),
      "--enable-blink-features=WebBluetooth,MiddleClickAutoscroll",
      // The pinned Brave's GTK4 startup calls a missing toolkit entry point.
      "--gtk-version=3",
      "--password-store=basic", "--window-size=1000,700", "--window-position=-16000,-16000"};
    if (!automation_mode) args.push_back("--disable-blink-features=AutomationControlled");
    // Consume the nested compositor's actual frame callbacks with upstream
    // in-flight backpressure. Uncapped rendering is only a diagnostic override.
    if (composited() && g_strcmp0(g_getenv("GNOSIS_UNCAP_FRAMES"), "1") == 0)
      args.push_back("--disable-frame-rate-limit");
    auto* launcher = g_subprocess_launcher_new(G_SUBPROCESS_FLAGS_NONE);
    g_subprocess_launcher_setenv(launcher, "GNOSIS_DISABLED_ADAPTERS", disabled_adapters().c_str(), true);
    // Brave deliberately searches native hosts under its default config path,
    // even with --user-data-dir. Keep that path inside this Gnosis profile.
    // The KeePassXC host launcher restores the original desktop environment.
    g_subprocess_launcher_setenv(launcher, "XDG_CONFIG_HOME", (profile + "/native-config").c_str(), true);
    if (composited()) {
      g_subprocess_launcher_setenv(launcher, "WAYLAND_DISPLAY", gnosis_compositor_get_socket(compositor), true);
      g_subprocess_launcher_setenv(launcher, "GDK_BACKEND", "wayland", true);
      g_subprocess_launcher_unsetenv(launcher, "WAYLAND_SOCKET");
      g_subprocess_launcher_unsetenv(launcher, "DISPLAY");
    }
    for (const char* name : {"LD_LIBRARY_PATH", "LD_PRELOAD", "CEF_ROOT"}) g_subprocess_launcher_unsetenv(launcher, name);
    const bool first = !launched;
    g_subprocess_launcher_set_stdout_file_path(launcher, (profile + (first ? "/stdout.log" : "/launcher-stdout.log")).c_str());
    g_subprocess_launcher_set_stderr_file_path(launcher, (profile + (first ? "/stderr.log" : "/launcher-stderr.log")).c_str());
    if (first) {
      int to_browser[2], from_browser[2];
      if (pipe2(to_browser, O_CLOEXEC) || pipe2(from_browser, O_CLOEXEC)) { g_object_unref(launcher); return; }
      output = to_browser[1]; input = from_browser[0]; fcntl(input, F_SETFL, O_NONBLOCK);
      g_subprocess_launcher_take_fd(launcher, to_browser[0], 3);
      g_subprocess_launcher_take_fd(launcher, from_browser[1], 4);
      // Only the pinned packaged runtime has the native presentation adapter.
      if (g_strcmp0(g_getenv("GNOSIS_NATIVE_UI_BRIDGE"), "1") == 0) {
        int sockets[2];
        if (!socketpair(AF_UNIX, SOCK_SEQPACKET | SOCK_CLOEXEC | SOCK_NONBLOCK, 0, sockets)) {
          native_fd = sockets[0];
          g_subprocess_launcher_take_fd(launcher, sockets[1], 5);
          g_subprocess_launcher_setenv(launcher, "GNOSIS_NATIVE_UI_FD", "5", true);
          native_watch = g_unix_fd_add(native_fd, GIOCondition(G_IO_IN | G_IO_HUP | G_IO_ERR),
            +[](gint, GIOCondition condition, gpointer pointer) -> gboolean {
              auto* self = static_cast<Process*>(pointer);
              if (condition & G_IO_IN) self->read_native_ui();
              if (self->native_fd < 0) { self->native_watch = 0; return G_SOURCE_REMOVE; }
              if (condition & (G_IO_HUP | G_IO_ERR)) {
                self->native_watch = 0; self->close_native_ui(); return G_SOURCE_REMOVE;
              }
              return G_SOURCE_CONTINUE;
            }, this);
        } else g_warning("Could not create the native install confirmation channel");
      }
      args.push_back("--remote-debugging-pipe"); args.push_back("--enable-unsafe-extension-debugging");
      // Optional source-built Brave honors this switch. The default prebuilt
      // overlay suppresses the native zoom bubble at its presentation entry.
      args.push_back("--gnosis-native-ui");
      launched = true;
    }
    std::vector<const char*> argv; for (auto& argument : args) argv.push_back(argument.c_str()); argv.push_back(nullptr);
    GError* error = nullptr; auto* child = g_subprocess_launcher_spawnv(launcher, argv.data(), &error); g_object_unref(launcher);
    if (!child) { g_warning("Could not start Brave: %s", error->message); g_error_free(error); return; }
    if (!first) { g_object_unref(child); return; }
    browser = child; browser_waiting = true;
    struct WaitingBrowser { std::weak_ptr<Process> process; std::shared_ptr<PrivateProfile> storage; };
    g_subprocess_wait_async(child,nullptr,+[](GObject* source,GAsyncResult* result,gpointer data) {
      std::unique_ptr<WaitingBrowser> waiting(static_cast<WaitingBrowser*>(data));
      g_autoptr(GError) error = nullptr;
      const bool exited = g_subprocess_wait_finish(G_SUBPROCESS(source),result,&error);
      if (auto self = waiting->process.lock()) {
        self->browser_waiting = !exited;
        if (!self->closing && self->browser == G_SUBPROCESS(source)) {
          if (error) g_warning("Brave process wait failed: %s",error->message);
          else if (g_subprocess_get_if_signaled(G_SUBPROCESS(source)))
            g_warning("Brave browser process died from signal %d",g_subprocess_get_term_sig(G_SUBPROCESS(source)));
          else g_warning("Brave browser process exited with status %d",g_subprocess_get_exit_status(G_SUBPROCESS(source)));
          self->disconnected();
        }
        if (exited) self->retire_private_profile();
      }
    },new WaitingBrowser{weak_from_this(), private_storage});
    watch = g_unix_fd_add(input, GIOCondition(G_IO_IN | G_IO_HUP | G_IO_ERR), +[](gint, GIOCondition condition, gpointer data) -> gboolean {
      auto* self = static_cast<Process*>(data); self->read();
      if (self->closing) { self->watch = 0; return G_SOURCE_REMOVE; }
      if (condition & (G_IO_HUP | G_IO_ERR)) {
        self->watch = 0;
        if (!self->restarting) self->disconnected(); // A restart waits for real process exit.
        return G_SOURCE_REMOVE;
      }
      return G_SOURCE_CONTINUE;
    }, this);
    command("Browser.getVersion", "{}", "", [this](JsonObject* result, const std::string& error) {
      if (!result) { g_warning("Brave startup: %s", error.c_str()); return; }
      product = text(result, "product"); ready = true;
      if (adapter_enabled("downloads")) command("Browser.setDownloadBehavior", "{\"behavior\":\"allowAndName\",\"downloadPath\":" + quote(download_directory) +
        ",\"eventsEnabled\":true}", "", [this](JsonObject*, const std::string& error) {
          if (!error.empty()) { fail_downloads(); g_warning("Could not connect Brave downloads: %s", error.c_str()); }
        });
      command("Target.setDiscoverTargets", "{\"discover\":true}", "");
      command("Extensions.loadUnpacked", "{\"path\":" + quote(GNOSIS_NATIVE_INTEGRATION_DIR) + "}", "", [this](JsonObject* result, const std::string& error) {
        if (!result) { integration_failed(error); return; }
        integration_id = text(result, "id");
        if (integration_id.empty()) { integration_failed("Native integration has no extension identity"); return; }
        if (integration_id != native_integration_id) { integration_failed("Native integration does not match its packaged identity"); return; }
        command("Target.getTargets", "{}", "", [this](JsonObject* result, const std::string&) {
          auto* list = result ? json_object_get_array_member(result, "targetInfos") : nullptr;
          for (guint i = 0; list && i < json_array_get_length(list); ++i) discovered(json_array_get_object_element(list, i));
        });
      });
      auto pending = std::move(pending_opens); pending_opens.clear();
      for (auto& weak : pending) if (auto page = weak.lock(); page && !page->is_closed()) open(page);
    });
  }
  void integration_failed(const std::string& error) {
    g_warning("Native browser UI handoff failed: %s", error.c_str());
    command("Browser.close", "{}", ""); disconnected();
  }
  void poll_integration() {
    if (closing || integration_ready) return;
    if (integration_adapter.empty()) {
      g_autofree char* path = g_build_filename(GNOSIS_NATIVE_INTEGRATION_DIR, "background.js", nullptr);
      g_autofree char* navigation_path = g_build_filename(GNOSIS_NATIVE_INTEGRATION_DIR, "navigation-target.js", nullptr);
      g_autofree char* source = nullptr; gsize length = 0;
      g_autofree char* navigation_source = nullptr; gsize navigation_length = 0;
      if (!g_file_get_contents(path, &source, &length, nullptr) || length > 64 * 1024 ||
          !g_file_get_contents(navigation_path, &navigation_source, &navigation_length, nullptr) || navigation_length > 64 * 1024) {
        integration_failed("The native integration adapter is missing"); return;
      }
      // An existing profile can retain a worker from before the namespace
      // upgrade. Install only our packaged adapter into this verified worker.
      integration_adapter = "(() => { globalThis.gnosisControlThemeEnabled=" + std::string(adapter_enabled("page-style") ? "true" : "false") + "; if (typeof globalThis.gnosisNativeReady === 'undefined' && "
        "typeof globalThis.gnosisExtensions === 'undefined' && globalThis.chrome?.downloads?.setUiOptions && "
        "globalThis.chrome?.management) { (() => {" + std::string(source, length) + "})(); } " +
        "if (globalThis.chrome?.webNavigation?.onCreatedNavigationTarget) { " + std::string(navigation_source, navigation_length) + " } " +
        "return Promise.resolve(globalThis.gnosisNativeReady).then(value => value === true); })()";
    }
    command("Runtime.evaluate", "{\"expression\":" + quote(integration_adapter) + ",\"awaitPromise\":true,\"returnByValue\":true}", integration_session,
      [this](JsonObject* result, const std::string& error) {
        if (closing) return;
        auto* details = object(result, "exceptionDetails");
        if (!result || details) {
          const std::string description = text(object(details, "exception"), "description");
          integration_failed(!error.empty() ? error : !description.empty() ? description : "Native integration worker failed"); return;
        }
        auto* value = object(result, "result");
        if (value && json_object_get_boolean_member_with_default(value, "value", false)) {
          apply_webapp_extensions([weak = weak_from_this()] {
            auto owner = weak.lock(); if (!owner || owner->closing) return;
            owner->apply_debug_extensions([weak] {
              auto self = weak.lock(); if (!self || self->closing) return;
              self->integration_ready = true;
              self->apply_preferences();
              if (self->extensions_ready) self->load_keepassxc();
              for (auto& [token, weak] : self->pages) if (auto page = weak.lock()) page->surface_ready();
            });
          });
          g_message("Brave download UI disabled: GTK owns download feedback");
          if (!private_profile) {
            extensions_deadline = g_get_monotonic_time() + 15 * G_USEC_PER_SEC;
            poll_extensions();
          }
          // Keep this private worker session alive for native browser services.
          return;
        }
        if (g_get_monotonic_time() >= integration_deadline) { integration_failed("Native integration worker did not become ready"); return; }
        integration_poll = g_timeout_add_full(G_PRIORITY_DEFAULT, 25, +[](gpointer data) -> gboolean {
          if (auto self = static_cast<std::weak_ptr<Process>*>(data)->lock()) {
            self->integration_poll = 0; self->poll_integration();
          }
          return G_SOURCE_REMOVE;
        }, new std::weak_ptr<Process>(weak_from_this()), +[](gpointer data) { delete static_cast<std::weak_ptr<Process>*>(data); });
      });
  }
  void apply_debug_extensions(std::function<void()> done) {
    const bool keepass_override = !private_profile && adapter_overridden("keepassxc");
    if (!keepass_override && adapter_enabled("downloads")) { done(); return; }
    std::string expression = "(async()=>{";
    if (!adapter_enabled("downloads")) expression += "await chrome.downloads.setUiOptions({enabled:true});";
    if (keepass_override) expression += "const item=(await chrome.management.getAll()).find(x=>x.id==='giohadoodmdcadnkcpdmhmmdnaidnckm');if(item&&item.enabled!==" +
      (adapter_enabled("keepassxc") && webapp_keepassxc ? std::string("true") : std::string("false")) + ")await chrome.management.setEnabled(item.id," +
      (adapter_enabled("keepassxc") && webapp_keepassxc ? "true" : "false") + ");";
    expression += "return true})()";
    command("Runtime.evaluate", "{\"expression\":" + quote(expression) + ",\"awaitPromise\":true,\"returnByValue\":true}", integration_session,
      [done = std::move(done)](JsonObject* result, const std::string& error) {
        if (!result || !error.empty() || object(result,"exceptionDetails")) g_warning("Could not apply browser debugging overrides: %s",error.c_str());
        done();
      });
  }
  void preferences_done() {
    preferences_busy = false;
    if (std::exchange(debug_startup_pending, false))
      for (auto& [token, weak] : pages) if (auto page = weak.lock()) page->surface_ready();
    if (std::exchange(preferences_again, false)) apply_preferences();
  }
  void disable_brave_adblock(unsigned attempts = 0) {
    if (closing) return;
    if (settings_target.empty()) {
      command("Target.createTarget", "{\"url\":\"brave://settings/shields\",\"newWindow\":true,\"background\":true}", "",
        [this](JsonObject* result, const std::string& error) {
          if (!result) { g_warning("Brave filtering handoff: %s", error.c_str()); preferences_done(); return; }
          settings_target = text(result, "targetId");
          command("Target.attachToTarget", "{\"targetId\":" + quote(settings_target) + ",\"flatten\":true}", "",
            [this](JsonObject* attached, const std::string&) {
              settings_session = text(attached, "sessionId"); disable_brave_adblock(1);
            });
        });
      return;
    }
    command("Runtime.evaluate", "{\"expression\":\"typeof chrome?.send === 'function' && location.protocol === 'chrome:' && (chrome.send('setAdControlType',[false]),chrome.send('setCosmeticFilteringControlType',['allow']),true)\",\"returnByValue\":true}", settings_session,
      [this, attempts](JsonObject* result, const std::string& error) {
        if (closing) return;
        auto* remote = object(result, "result");
        bool done = remote && json_object_get_boolean_member_with_default(remote, "value", false);
        if (!done && attempts < 50 && error.empty()) {
          g_timeout_add_full(G_PRIORITY_DEFAULT, 100, +[](gpointer data) -> gboolean {
            auto* pending = static_cast<std::pair<std::weak_ptr<Process>, unsigned>*>(data);
            if (auto self = pending->first.lock()) self->disable_brave_adblock(pending->second);
            return G_SOURCE_REMOVE;
          }, new std::pair<std::weak_ptr<Process>, unsigned>(weak_from_this(), attempts + 1),
          +[](gpointer data) { delete static_cast<std::pair<std::weak_ptr<Process>, unsigned>*>(data); });
          return;
        }
        shields_configured = done;
        if (!done) g_warning("Could not hand Brave ad filtering to uBlock Origin: %s", error.c_str());
        command("Target.closeTarget", "{\"targetId\":" + quote(settings_target) + "}", "");
        settings_target.clear(); settings_session.clear(); preferences_done();
      });
  }
#include "bundled-extension-runtime.inc"
  void apply_preferences() {
    if (closing || !integration_ready) return;
    if (preferences_busy) { preferences_again = true; return; }
    preferences_busy = true;
    g_autofree char* path = g_build_filename(GNOSIS_NATIVE_INTEGRATION_DIR, "settings.js", nullptr);
    g_autofree char* source = nullptr; gsize size = 0;
    if (!g_file_get_contents(path, &source, &size, nullptr) || size > 64 * 1024) { preferences_done(); return; }
    auto parser = json_parser_new(); json_parser_load_from_data(parser, native_preferences.c_str(), -1, nullptr);
    auto* prefs = json_parser_get_root(parser);
    bool blocking = webapp_ublock && adapter_enabled("ublock") && prefs && JSON_NODE_HOLDS_OBJECT(prefs) && json_object_get_boolean_member_with_default(json_node_get_object(prefs), "adblock", true);
    g_object_unref(parser);
    const auto expression = std::string(source, size) + (adapter_enabled("browser-preferences") ? ";gnosisBrowserSettings.apply(" + native_preferences + ")" : ";Promise.resolve()") + ".then(()=>gnosisBrowserSettings.adblock(" + (blocking ? "true" : "false") + ")).then(()=>chrome.management.getAll())";
    command("Runtime.evaluate", "{\"expression\":" + quote(expression) + ",\"awaitPromise\":true,\"returnByValue\":true}", integration_session,
      [this, blocking](JsonObject* result, const std::string& error) {
        if (!error.empty() || object(result, "exceptionDetails")) {
          g_warning("Brave preferences could not be applied: %s %s", error.c_str(), text(object(object(result, "exceptionDetails"), "exception"), "description"));
          preferences_done(); return;
        }
        auto finish = [this] { if (!shields_configured && adapter_enabled("browser-preferences")) disable_brave_adblock(); else preferences_done(); };
        auto* rows = object(result, "result");
        auto* value = rows ? json_object_get_member(rows, "value") : nullptr;
        if (!value || !JSON_NODE_HOLDS_ARRAY(value)) { preferences_done(); return; }
        refresh_bundled_adblock(json_node_get_array(value), blocking, std::move(finish));
      });
  }
  void poll_extensions() {
    if (closing || private_profile || extensions_ready) return;
    command("Runtime.evaluate", "{\"expression\":\"typeof globalThis.gnosisExtensions?.inventory === 'function'\",\"returnByValue\":true}", integration_session,
      [this](JsonObject* result, const std::string& error) {
        if (closing) return;
        auto* remote = object(result, "result");
        if (remote && json_object_get_boolean_member_with_default(remote, "value", false)) {
          extensions_ready = true;
          g_message("Brave native extension manager ready");
          if (extensions_changed) extensions_changed();
          if (integration_ready) load_keepassxc();
          return;
        }
        if (g_get_monotonic_time() >= extensions_deadline) {
          g_warning("Brave extension manager unavailable; browsing remains active: %s", error.c_str()); return;
        }
        extensions_poll = g_timeout_add_full(G_PRIORITY_DEFAULT, 100, +[](gpointer data) -> gboolean {
          if (auto self = static_cast<std::weak_ptr<Process>*>(data)->lock()) {
            self->extensions_poll = 0; self->poll_extensions();
          }
          return G_SOURCE_REMOVE;
        }, new std::weak_ptr<Process>(weak_from_this()), +[](gpointer data) { delete static_cast<std::weak_ptr<Process>*>(data); });
      });
  }
  void load_keepassxc() {
    if (private_profile || !webapp_keepassxc || !adapter_enabled("keepassxc") || !*GNOSIS_KEEPASSXC_DIR) return;
    extension_operation("inventory", "", [this](JsonObject* result, const std::string& error) {
      if (closing || !error.empty()) return;
      auto* value = object(result, "result");
      auto* node = value ? json_object_get_member(value, "value") : nullptr;
      if (!node || !JSON_NODE_HOLDS_ARRAY(node)) return;
      auto* list = json_node_get_array(node);
      for (guint i = 0; i < json_array_get_length(list); ++i) {
        auto* item = json_array_get_object_element(list, i);
        if (std::string(text(item, "id")) == "giohadoodmdcadnkcpdmhmmdnaidnckm" &&
            !json_object_get_boolean_member_with_default(item, "enabled", false)) return;
      }
      command("Extensions.loadUnpacked", "{\"path\":" + quote(GNOSIS_KEEPASSXC_DIR) + "}", "",
        [this](JsonObject* result, const std::string& error) {
          if (!result || std::string(text(result, "id")) != "giohadoodmdcadnkcpdmhmmdnaidnckm")
            g_warning("Could not load bundled KeePassXC-Browser: %s", error.c_str());
          else {
            keepassxc_deadline = g_get_monotonic_time() + 15 * G_TIME_SPAN_SECOND;
            connect_keepassxc();
          }
        });
    });
  }
  void connect_keepassxc() {
    if (closing || private_profile || !keepassxc_session.empty()) return;
    command("Target.getTargets", "{}", "", [this](JsonObject* result, const std::string&) {
      if (closing) return;
      auto* list = result ? json_object_get_array_member(result, "targetInfos") : nullptr;
      for (guint i = 0; list && i < json_array_get_length(list); ++i) {
        auto* info = json_array_get_object_element(list, i);
        if (std::string(text(info, "type")) != "service_worker" ||
            std::string(text(info, "url")) != "chrome-extension://giohadoodmdcadnkcpdmhmmdnaidnckm/background/background_service.js") continue;
        keepassxc_target = text(info, "targetId");
        command("Target.attachToTarget", "{\"flatten\":true,\"targetId\":" + quote(keepassxc_target) + "}", "",
          [this](JsonObject* result, const std::string& error) {
            keepassxc_session = text(result, "sessionId");
            if (keepassxc_session.empty()) { g_warning("KeePassXC service: %s", error.c_str()); return; }
            const auto candidate = keepassxc_session;
            command("Runtime.enable", "{}", candidate);
            command("Runtime.addBinding", "{\"name\":\"__gnosisPasskeyProvider\"}", candidate);
            command("Runtime.addBinding", "{\"name\":\"__gnosisKeePassXCPairingChanged\"}", candidate);
            command("Runtime.runIfWaitingForDebugger", "{}", candidate);
            // An existing profile can keep an older worker script after a
            // resource-only unpacked-extension update. Install only our trusted
            // adapter into this verified worker; do not reset extension storage
            // or worker caches belonging to other extensions/websites.
            g_autofree char* adapter_path = g_build_filename(GNOSIS_KEEPASSXC_DIR, "background", "gnosis.js", nullptr);
            g_autofree char* adapter = nullptr; gsize length = 0;
            if (!g_file_get_contents(adapter_path, &adapter, &length, nullptr) || length > 64 * 1024) {
              keepassxc_session.clear(); g_warning("KeePassXC adapter is missing"); return;
            }
            const std::string expression = "(() => { if (typeof page !== 'object' || !page.settings || typeof keepass !== 'object') return false; " +
              std::string(adapter, length) + "; return typeof globalThis.gnosisKeePassXC?.invoke === 'function'; })()";
            command("Runtime.evaluate", "{\"expression\":" + quote(expression) + ",\"returnByValue\":true}", candidate,
              [this, candidate](JsonObject* result, const std::string&) {
                if (closing) return;
                auto* remote = object(result, "result");
                if (remote && json_object_get_boolean_member_with_default(remote, "value", false)) {
                  keepassxc_ready = true; g_message("Native KeePassXC service ready");
                  start_keepassxc_pairing();
                  return;
                }
                command("Target.detachFromTarget", "{\"sessionId\":" + quote(candidate) + "}", "");
                keepassxc_session.clear();
                if (g_get_monotonic_time() >= keepassxc_deadline) { g_warning("KeePassXC adapter did not initialize"); return; }
                auto* weak = new std::weak_ptr<Process>(weak_from_this());
                keepassxc_poll = g_timeout_add_full(G_PRIORITY_DEFAULT, 200, +[](gpointer data) -> gboolean {
                  if (auto process = static_cast<std::weak_ptr<Process>*>(data)->lock()) {
                    process->keepassxc_poll = 0; process->connect_keepassxc();
                  }
                  return G_SOURCE_REMOVE;
                }, weak, +[](gpointer data) { delete static_cast<std::weak_ptr<Process>*>(data); });
              });
          });
        return;
      }
      if (g_get_monotonic_time() >= keepassxc_deadline) { g_warning("KeePassXC service did not start; browsing remains available"); return; }
      auto* weak = new std::weak_ptr<Process>(weak_from_this());
      keepassxc_poll = g_timeout_add_full(G_PRIORITY_DEFAULT, 100, +[](gpointer data) -> gboolean {
        if (auto process = static_cast<std::weak_ptr<Process>*>(data)->lock()) {
          process->keepassxc_poll = 0; process->connect_keepassxc();
        }
        return G_SOURCE_REMOVE;
      }, weak, +[](gpointer data) { delete static_cast<std::weak_ptr<Process>*>(data); });
    });
  }
  void keepassxc_call(int tab, const std::string& operation, const std::string& argument, Reply reply) {
    if (private_profile || !keepassxc_ready || keepassxc_session.empty()) { reply(nullptr, _("KeePassXC is starting or unavailable. Try again shortly.")); return; }
    const std::string expression = "gnosisKeePassXC.invoke(" + std::to_string(tab) + "," + quote(operation) + "," + argument + ")";
    command("Runtime.evaluate", "{\"expression\":" + quote(expression) + ",\"awaitPromise\":true,\"returnByValue\":true,\"userGesture\":true}", keepassxc_session,
      [reply = std::move(reply)](JsonObject* result, const std::string& error) {
        auto* exception = object(result, "exceptionDetails");
        auto* value = object(object(result, "result"), "value");
        if (!error.empty() || exception || !value) {
          const auto detail = std::string(text(object(exception, "exception"), "description"));
          reply(nullptr, !error.empty() ? error : !detail.empty() ? detail : _("KeePassXC did not respond"));
        } else reply(value, "");
      });
  }
  void extension_operation(const std::string& method, const std::string& arguments, Reply reply) {
    command("Runtime.evaluate", "{\"expression\":" + quote("gnosisExtensions." + method + "(" + arguments + ")") +
      ",\"awaitPromise\":true,\"returnByValue\":true,\"userGesture\":true}", integration_session,
      [reply = std::move(reply)](JsonObject* result, const std::string& error) {
        auto* exception = object(result, "exceptionDetails");
        if (!result || exception) {
          const std::string description = text(object(exception, "exception"), "description");
          reply(nullptr, !error.empty() ? error : !description.empty() ? description : "Extension operation failed");
        } else reply(result, "");
      });
  }
  void discovered(JsonObject* info) {
    if (!integration_ready && !integration_connecting && !integration_id.empty() &&
        std::string(text(info, "type")) == "service_worker" &&
        std::string(text(info, "url")) == "chrome-extension://" + integration_id + "/background.js") {
      integration_connecting = true;
      command("Target.attachToTarget", "{\"targetId\":" + quote(text(info, "targetId")) + ",\"flatten\":true}", "",
        [this](JsonObject* result, const std::string& error) {
          if (!result) { integration_failed(error); return; }
          integration_session = text(result, "sessionId");
          integration_deadline = g_get_monotonic_time() + 5 * G_USEC_PER_SEC;
          command("Runtime.enable", "{}", integration_session);
          command("Runtime.addBinding", "{\"name\":\"__gnosisZoomChanged\"}",integration_session);
          command("Runtime.addBinding", "{\"name\":\"__gnosisNavigationTargetCreated\"}", integration_session);
          command("Runtime.addBinding", "{\"name\":\"__gnosisExtensionsChanged\"}", integration_session);
          command("Runtime.runIfWaitingForDebugger", "{}", integration_session);
          poll_integration();
        });
    }
    if (strcmp(text(info, "type"), "page")) return;
    const std::string uri = text(info, "url");
    for (auto& [token, weak] : pages) if (auto page = weak.lock()) {
      if (!page->target.empty() || (page->state_->bootstrap != uri &&
          std::string(text(info, "title")) != "GnosisTab-" + token)) continue;
      page->target = text(info, "targetId"); targets[page->target] = page;
      g_message("Brave target connected: token=%s target=%s", token.c_str(), page->target.c_str());
      auto args = "{\"targetId\":" + quote(page->target) + ",\"flatten\":true}";
      command("Target.attachToTarget", args, "", [this, weak](JsonObject* result, const std::string&) {
        auto page = weak.lock(); if (!page || page->state_->disposed || !result) return;
        page->session = text(result, "sessionId"); sessions[page->session] = page; page->attach();
      });
      return;
    }
  }
  void read() {
    char chunk[65536];
    for (;;) {
      ssize_t size = ::read(input, chunk, sizeof chunk);
      if (size < 0 && errno == EINTR) continue;
      if (size <= 0) break;
      buffer.append(chunk, size);
    }
    for (;;) {
      const auto end = buffer.find('\0'); if (end == std::string::npos) break;
      std::string json = buffer.substr(0, end); buffer.erase(0, end + 1);
      if (restarting) continue; // Native target-destroyed events must not close GTK tabs.
      auto* parser = json_parser_new();
      if (json_parser_load_from_data(parser, json.data(), json.size(), nullptr) && JSON_NODE_HOLDS_OBJECT(json_parser_get_root(parser))) {
        auto* root = json_node_get_object(json_parser_get_root(parser));
        if (json_object_has_member(root, "id")) {
          int id = json_object_get_int_member(root, "id"); auto found = replies.find(id);
          if (found != replies.end()) {
            auto reply = std::move(found->second); replies.erase(found);
            auto* failure = object(root, "error"); reply(object(root, "result"), failure ? text(failure, "message") : "");
          }
        } else {
          const std::string method = text(root, "method"); auto* params = object(root, "params");
          if (method == "Browser.downloadWillBegin") {
            const std::string guid = text(params, "guid");
            // Browser events can be delivered through more than one attached
            // session. Each transfer must create exactly one native object.
            if (download_guids.contains(guid)) { g_object_unref(parser); continue; }
            download_guids.insert(guid);
            auto found = frame_owners.find(text(params, "frameId"));
            auto page = found == frame_owners.end() ? nullptr : found->second.lock();
            if (!g_uuid_string_is_valid(guid.c_str()) || !page || page->is_closed() || !page->download_begin) {
              g_message("Brave download rejected: valid-guid=%d known-frame=%d active-page=%d", g_uuid_string_is_valid(guid.c_str()),
                found != frame_owners.end(), page && !page->is_closed());
              command("Browser.cancelDownload", "{\"guid\":" + quote(guid) + "}", "");
            } else {
              auto weak = weak_from_this();
              auto progress = page->download_begin(text(params, "url"), text(params, "suggestedFilename"), download_directory + '/' + guid,
                [weak, guid] { if (auto self = weak.lock()) self->command("Browser.cancelDownload", "{\"guid\":" + quote(guid) + "}", ""); });
              if (progress) downloads[guid] = std::move(progress);
              else command("Browser.cancelDownload", "{\"guid\":" + quote(guid) + "}", "");
            }
          } else if (method == "Browser.downloadProgress") {
            auto found = downloads.find(text(params, "guid"));
            if (found != downloads.end()) {
              auto progress = found->second;
              const std::string state = text(params, "state");
              if (state != "inProgress") downloads.erase(found);
              progress(static_cast<guint64>(std::max(0., json_object_get_double_member_with_default(params, "receivedBytes", 0))),
                       static_cast<guint64>(std::max(0., json_object_get_double_member_with_default(params, "totalBytes", 0))), state);
            }
          } else if (method == "Runtime.bindingCalled" && !private_profile && keepassxc_ready && keepassxc_pairing_publisher &&
                     std::string(text(root,"sessionId")) == keepassxc_session &&
                     std::string(text(params,"name")) == "__gnosisKeePassXCPairingChanged") {
            const std::string json = text(params,"payload");
            if (json != keepassxc_pairing_json) {
              g_autoptr(GError) error = nullptr;
              if (gnosis_brave_keepassxc_pairing_save(keepassxc_pairing_path.c_str(), json.c_str(), &error))
                keepassxc_pairing_json = json;
              else g_warning("KeePassXC association sharing: %s", error->message);
            }
          } else if (method == "Runtime.bindingCalled" && !private_profile && keepassxc_ready &&
                     std::string(text(root,"sessionId")) == keepassxc_session &&
                     std::string(text(params,"name")) == "__gnosisPasskeyProvider") {
            const std::string payload = text(params,"payload");
            g_autoptr(JsonParser) event = json_parser_new();
            if (payload.size() <= 8192 && json_parser_load_from_data(event,payload.data(),payload.size(),nullptr) &&
                JSON_NODE_HOLDS_OBJECT(json_parser_get_root(event))) {
              auto* request = json_node_get_object(json_parser_get_root(event));
              const auto id = std::string(text(request,"id"));
              if (id.size() != 36 || id.find_first_not_of("0123456789abcdef-") != std::string::npos) continue;
              const int tab = json_object_get_int_member_with_default(request,"tabId",-1);
              if (json_object_get_boolean_member_with_default(request,"closed",false)) {
                for (auto& [token, weak] : pages) if (auto page = weak.lock(); page &&
                    page->state_->native_tab == tab && page->state_->passkey_provider_id == id)
                  page->dismiss_passkey_provider();
                continue;
              }
              bool shown = false;
              for (auto& [token, weak] : pages) if (auto page = weak.lock(); page && !page->is_closed() &&
                  page->state_->mapped && page->state_->native_tab == tab && page->uri == text(request,"url")) {
                page->show_passkey_provider(request); shown = true; break;
              }
              if (!shown) command("Runtime.evaluate", "{\"expression\":" + quote("gnosisKeePassXC.resolvePasskey(" + quote(id) + ", 'cancel')") + "}", keepassxc_session);
            }
          } else if (method == "Runtime.bindingCalled" && !private_profile && extensions_ready &&
                     std::string(text(root,"sessionId")) == integration_session &&
                     std::string(text(params,"name")) == "__gnosisExtensionsChanged") {
            if (extensions_changed) extensions_changed();
          } else if (method == "Runtime.bindingCalled" && std::string(text(root,"sessionId")) == integration_session &&
                     std::string(text(params,"name")) == "__gnosisZoomChanged") {
            const std::string payload = text(params,"payload");
            g_autoptr(JsonParser) event = json_parser_new();
            if (payload.size() <= 1024 && json_parser_load_from_data(event,payload.data(),payload.size(),nullptr) &&
                JSON_NODE_HOLDS_OBJECT(json_parser_get_root(event))) {
              auto* change = json_node_get_object(json_parser_get_root(event));
              auto found = zoom_tabs.find(json_object_get_int_member_with_default(change,"tabId",-1));
              if (found != zoom_tabs.end() && std::string(text(change,"scope")) == "per-tab") if (auto page = found->second.lock())
                page->receive_zoom(json_object_get_double_member_with_default(change,"factor",0));
            }
          } else if (method == "Runtime.bindingCalled" && std::string(text(root,"sessionId")) == integration_session &&
                     std::string(text(params,"name")) == "__gnosisNavigationTargetCreated") {
            const std::string payload = text(params,"payload");
            g_autoptr(JsonParser) event = json_parser_new();
            if (payload.size() <= 65536 && json_parser_load_from_data(event,payload.data(),payload.size(),nullptr) &&
                JSON_NODE_HOLDS_OBJECT(json_parser_get_root(event))) {
              auto* created = json_node_get_object(json_parser_get_root(event));
              const int source = json_object_get_int_member_with_default(created,"sourceTabId",-1);
              const int tab = json_object_get_int_member_with_default(created,"tabId",-1);
              const std::string uri = text(created,"url");
              auto found = zoom_tabs.find(source);
              if (tab >= 0 && tab != source && found != zoom_tabs.end() &&
                  (uri.starts_with("http://") || uri.starts_with("https://"))) {
                if (auto page = found->second.lock(); page && !page->is_closed() && page->open_uri && page->open_uri(uri)) {
                  command("Runtime.evaluate", "{\"expression\":" + quote("chrome.tabs.remove(" + std::to_string(tab) + ").catch(()=>{})") + "}", integration_session);
                  g_message("Brave link tab routed into the GTK shell");
                }
              }
            }
          } else if (method == "Target.targetCreated") discovered(object(params, "targetInfo"));
          else if (method == "Target.targetInfoChanged") {
            auto* info = object(params, "targetInfo"); discovered(info); auto found = targets.find(text(info, "targetId"));
            if (found != targets.end()) if (auto page = found->second.lock()) {
              if (page->state_->attached) {
                page->uri = page->canonical_uri(text(info, "url"));
                if (page->state_->document_title_uri != page->uri) {
                  const std::string candidate = text(info, "title");
                  page->title = candidate != text(info, "url") && !candidate.starts_with("GnosisTab-") ? candidate : "";
                }
                if (page->changed) page->changed();
              }
            }
          } else if (method == "Target.targetCrashed") {
            g_warning("Brave renderer crashed: target=%s status=%s error=%d",text(params,"targetId"),text(params,"status"),
              int(json_object_get_int_member_with_default(params,"errorCode",0)));
            auto found = targets.find(text(params,"targetId"));
            if (found != targets.end()) if (auto page = found->second.lock()) {
              page->loading = false; page->progress = 0; page->dismiss_context_menu();
              page->dismiss_script_dialog(false);
              if (page->changed) page->changed();
            }
          } else if (method == "Target.targetDestroyed") {
            if (keepassxc_target == text(params, "targetId")) { keepassxc_ready = false; keepassxc_session.clear(); keepassxc_target.clear(); }
            for (auto& [token, weak] : pages) if (auto owner = weak.lock())
              if (owner->state_->devtools_target == text(params, "targetId")) {
                owner->state_->devtools_target.clear(); owner->close_devtools();
              }
            auto found = targets.find(text(params, "targetId"));
            if (found != targets.end()) {
              auto page = found->second.lock();
              targets.erase(found);
              if (page && !page->state_->disposed) {
                page->state_->native_destroyed = true;
                page->dismiss_script_dialog(false);
                if (page->closed) page->closed();
              }
            }
            std::erase_if(parked, [&](const auto& page) {
              if (page.target != text(params, "targetId")) return false;
              if (page.surface) g_object_unref(page.surface);
              return true;
            });
          }
          const std::string source = text(root,"sessionId");
          if (!frame_event(source,method,params)) {
            auto found = sessions.find(source);
            if (found != sessions.end()) if (auto page = found->second.lock()) page->handle(method, params);
          }
        }
      }
      g_object_unref(parser);
    }
    if (buffer.size() > 64 * 1024 * 1024) { buffer.clear(); fail("Oversized Brave protocol message"); }
  }
#include "frame-process.inc"
#include "find-process.inc"
#include "close-process.inc"
  void translation_request(const std::string& target, const std::string& operation,
                           const std::string& language, int page_sequence, Reply callback) {
    if (native_fd < 0) { callback(nullptr, _("The translation service is unavailable")); return; }
    const auto id = ++native_sequence;
    struct Timeout { std::weak_ptr<Process> process; gint64 id; };
    auto* timeout = new Timeout{weak_from_this(), id};
    guint source = g_timeout_add_full(G_PRIORITY_DEFAULT, 5000, +[](gpointer data) -> gboolean {
      auto* timeout = static_cast<Timeout*>(data);
      if (auto process = timeout->process.lock()) {
        auto found = process->native_replies.find(timeout->id);
        if (found != process->native_replies.end()) {
          auto callback = std::move(found->second.callback); process->native_replies.erase(found);
          callback(nullptr, _("The translation service did not respond"));
        }
      }
      return G_SOURCE_REMOVE;
    }, timeout, +[](gpointer data) { delete static_cast<Timeout*>(data); });
    native_replies.emplace(id, NativeReply{std::move(callback), source});
    const std::string message = "{\"kind\":\"translation\",\"id\":" + std::to_string(id) +
      ",\"target\":" + quote(target) + ",\"operation\":" + quote(operation) +
      ",\"language\":" + quote(language) + ",\"sequence\":" + std::to_string(page_sequence) + "}";
    if (send(native_fd, message.data(), message.size(), MSG_NOSIGNAL | MSG_DONTWAIT) != static_cast<ssize_t>(message.size()))
      close_native_ui();
  }
  void native_decide(gint64 id, bool accept, const std::string& choice = "") {
    if (native_fd < 0 || id <= 0) return;
    std::string message = "{\"id\":" + std::to_string(id) + ",\"accept\":" + (accept ? "true" : "false") + ",\"choice\":" + quote(choice) + "}";
    if (send(native_fd, message.data(), message.size(), MSG_NOSIGNAL | MSG_DONTWAIT) != static_cast<ssize_t>(message.size()))
      close_native_ui(); // HUP makes the native adapter cancel pending requests.
  }
  void close_native_ui() {
    if (native_watch) { g_source_remove(native_watch); native_watch = 0; }
    if (native_fd >= 0) ::close(native_fd);
    native_fd = -1;
    auto replies = std::move(native_replies); native_replies.clear();
    for (auto& [id, reply] : replies) {
      if (reply.timeout) g_source_remove(reply.timeout); reply.callback(nullptr, _("The translation service stopped"));
    }
    for (auto& [token, weak] : pages) if (auto page = weak.lock()) {
      page->dismiss_install_dialog(false); page->dismiss_authenticator(false); page->dismiss_passkey_provider();
    }
  }
  bool dispatch_authenticator(JsonObject* message, gint64 id) {
    const auto revision = json_object_get_int_member_with_default(message, "revision", 0);
    auto* options = json_object_get_member(message, "options");
    bool valid = revision > 0 && options && JSON_NODE_HOLDS_ARRAY(options) && json_array_get_length(json_node_get_array(options)) <= 64;
    for (const char* key : {"title", "description", "rp", "accept", "cancel", "other", "target"})
      valid = valid && json_object_has_member(message, key) && json_node_get_value_type(json_object_get_member(message, key)) == G_TYPE_STRING;
    for (const char* key : {"enabled", "busy", "cancel_visible", "other_visible", "security_key", "create"})
      valid = valid && json_object_has_member(message, key) && json_node_get_value_type(json_object_get_member(message, key)) == G_TYPE_BOOLEAN;
    for (const char* key : {"revision", "step", "accept_state"})
      valid = valid && json_object_has_member(message, key) && json_node_get_value_type(json_object_get_member(message, key)) == G_TYPE_INT64;
    if (!valid) return false;
    const int accept_state = json_object_get_int_member(message, "accept_state");
    const int step = json_object_get_int_member(message, "step");
    if (accept_state < 0 || accept_state > 3 || step < 3 || step > 54) return false;
    auto* array = json_node_get_array(options);
    for (guint i = 0; i < json_array_get_length(array); ++i) {
      auto* node = json_array_get_element(array, i);
      if (!JSON_NODE_HOLDS_OBJECT(node)) return false;
      auto* item = json_node_get_object(node);
      for (const char* key : {"label", "description"})
        if (!json_object_has_member(item, key) || json_node_get_value_type(json_object_get_member(item, key)) != G_TYPE_STRING) return false;
      auto* index = json_object_get_member(item, "index");
      if (!index || json_node_get_value_type(index) != G_TYPE_INT64 || json_node_get_int(index) < 0 || json_node_get_int(index) > 63) return false;
    }
    auto* qr = json_object_get_member(message, "qr");
    if (qr && (json_node_get_value_type(qr) != G_TYPE_STRING || strlen(json_node_get_string(qr)) > 8192 ||
               !g_str_has_prefix(json_node_get_string(qr), "FIDO:/"))) return false;
    if (step == 20 && !qr) return false;
    auto* notes = json_object_get_member(message, "notes");
    if (!notes || !JSON_NODE_HOLDS_ARRAY(notes) || json_array_get_length(json_node_get_array(notes)) > 64) return false;
    for (guint i = 0; i < json_array_get_length(json_node_get_array(notes)); ++i) {
      auto* note = json_array_get_element(json_node_get_array(notes), i);
      if (json_node_get_value_type(note) != G_TYPE_STRING || strlen(json_node_get_string(note)) > 8192) return false;
    }
    std::shared_ptr<Page> owner;
    auto found = targets.find(text(message, "target"));
    if (found != targets.end()) owner = found->second.lock();
    if (owner && (owner->is_closed() || !owner->state_->mapped || !gtk_widget_get_mapped(owner->widget()))) owner.reset();
    if (owner) owner->show_authenticator(message); else native_decide(id, false);
    return true;
  }
  void read_native_ui() {
    for (;;) {
      char data[65536];
      ssize_t length = recv(native_fd, data, sizeof data, MSG_DONTWAIT | MSG_TRUNC);
      if (length < 0 && errno == EINTR) continue;
      if (length < 0 && errno == EAGAIN) return;
      if (length <= 0 || length > static_cast<ssize_t>(sizeof data)) { close_native_ui(); return; }
      g_autoptr(JsonParser) parser = json_parser_new();
      if (!json_parser_load_from_data(parser, data, length, nullptr) ||
          !JSON_NODE_HOLDS_OBJECT(json_parser_get_root(parser))) { close_native_ui(); return; }
      auto* message = json_node_get_object(json_parser_get_root(parser));
      auto* id_node = json_object_get_member(message, "id");
      auto* kind_node = json_object_get_member(message, "kind");
      if (!kind_node || json_node_get_value_type(kind_node) != G_TYPE_STRING) { close_native_ui(); return; }
      const std::string kind = json_node_get_string(kind_node);
      if (kind == "restart") { restart(); continue; }
      if (!id_node || json_node_get_value_type(id_node) != G_TYPE_INT64) { close_native_ui(); return; }
      const gint64 id = json_node_get_int(id_node);
      if (id <= 0) { close_native_ui(); return; }
      if (kind == "authenticator") {
        if (!dispatch_authenticator(message, id)) { close_native_ui(); return; }
        continue;
      }
      if (kind == "translation" || kind == "find-result" || kind == "beforeunload-result") {
        auto found = native_replies.find(id);
        if (found != native_replies.end()) {
          auto reply = std::move(found->second); native_replies.erase(found);
          if (reply.timeout) g_source_remove(reply.timeout); reply.callback(message, text(message, "error"));
        }
        continue;
      }
      if (kind == "closed") {
        for (auto& [token, weak] : pages) if (auto page = weak.lock(); page && page->state_->authenticator && page->state_->authenticator->id == id) page->dismiss_authenticator(false);
        for (auto& [token, weak] : pages) if (auto page = weak.lock(); page && page->state_->install_id == id)
          page->dismiss_install_dialog(false);
        continue;
      }
      if (kind == "permission") {
        auto* title = json_object_get_member(message, "title");
        auto* once = json_object_get_member(message, "once");
        if (!title || json_node_get_value_type(title) != G_TYPE_STRING ||
            !once || json_node_get_value_type(once) != G_TYPE_BOOLEAN) { close_native_ui(); return; }
        json_object_set_string_member(message, "accept", json_node_get_boolean(once) ? _("Allow on Every Visit") : _("Allow"));
        json_object_set_string_member(message, "cancel", _("Not Now"));
        json_object_set_string_member(message, "heading", "");
        json_object_set_array_member(message, "permissions", json_array_new());
      } else if (kind == "chooser") {
        auto* options = json_object_get_member(message, "options");
        auto* revision = json_object_get_member(message, "revision");
        auto* multiple = json_object_get_member(message, "multiple");
        auto* enabled = json_object_get_member(message, "enabled");
        if (!options || !JSON_NODE_HOLDS_ARRAY(options) || json_array_get_length(json_node_get_array(options)) > 256 ||
            !revision || json_node_get_value_type(revision) != G_TYPE_INT64 || json_node_get_int(revision) <= 0 ||
            !multiple || json_node_get_value_type(multiple) != G_TYPE_BOOLEAN ||
            !enabled || json_node_get_value_type(enabled) != G_TYPE_BOOLEAN) { close_native_ui(); return; }
        for (guint i = 0; i < json_array_get_length(json_node_get_array(options)); ++i)
          if (json_node_get_value_type(json_array_get_element(json_node_get_array(options), i)) != G_TYPE_STRING) { close_native_ui(); return; }
      } else if (kind == "uninstall") {
        if (!*text(message, "cancel")) json_object_set_string_member(message, "cancel", _("Cancel"));
        auto* checkbox = json_object_get_member(message, "checkbox");
        auto* checked = json_object_get_member(message, "checked");
        if ((checkbox && json_node_get_value_type(checkbox) != G_TYPE_STRING) ||
            (checkbox && (!checked || json_node_get_value_type(checked) != G_TYPE_BOOLEAN))) { close_native_ui(); return; }
      } else if (kind != "install" && kind != "file-access") { close_native_ui(); return; }
      for (const char* name : {"title", "accept", "cancel", "heading"}) {
        auto* node = json_object_get_member(message, name);
        if (!node || json_node_get_value_type(node) != G_TYPE_STRING) { close_native_ui(); return; }
      }
      auto* warnings = json_object_get_member(message, "permissions");
      if (!warnings || !JSON_NODE_HOLDS_ARRAY(warnings)) { close_native_ui(); return; }
      auto* list = json_node_get_array(warnings);
      if (json_array_get_length(list) > 256) { close_native_ui(); return; }
      for (guint i = 0; i < json_array_get_length(list); ++i)
        if (json_node_get_value_type(json_array_get_element(list, i)) != G_TYPE_STRING) { close_native_ui(); return; }
      if (kind == "chooser") {
        bool updated = false;
        for (auto& [token, weak] : pages) if (auto page = weak.lock(); page && page->state_->install_id == id) {
          page->show_install_dialog(message); updated = true; break;
        }
        if (updated) continue;
      }
      // This is a browser-owned confirmation. Use only this process's visible
      // page in the active GTK window; never borrow a private/normal window.
      std::shared_ptr<Page> owner;
      auto* application = g_application_get_default();
      auto* active = GTK_IS_APPLICATION(application) ? gtk_application_get_active_window(GTK_APPLICATION(application)) : nullptr;
      for (auto& [token, weak] : pages) if (auto page = weak.lock(); page && !page->is_closed() &&
          page->state_->mapped && gtk_widget_get_mapped(page->widget()) && active &&
          gtk_widget_get_root(page->widget()) == GTK_ROOT(active)) {
        if (owner) { owner.reset(); break; }
        owner = page;
      }
      if (!owner) {
        g_warning("Native browser confirmation cancelled: no unique visible page in the active GTK window");
        native_decide(id, false);
      } else {
        g_message("Native %s confirmation routed to visible GTK tab", kind.c_str());
        owner->show_install_dialog(message);
      }
    }
  }
  void retire_private_profile() {
    if (!private_profile || browser_waiting || profile_writers || restarting) return;
    private_storage.reset();
    std::erase(retiring_private_processes, shared_from_this());
  }
  void drain_private_shutdown() {
    if (!private_profile) return;
    const auto deadline = g_get_monotonic_time() + 5 * G_USEC_PER_SEC;
    while ((browser_waiting || profile_writers) && g_get_monotonic_time() < deadline) {
      while (g_main_context_iteration(nullptr, false)) {}
      g_usleep(1000);
    }
    if (!browser_waiting && !profile_writers) retire_private_profile();
    else g_warning("Private browser has not exited; retaining its owned directory until its writers stop");
  }
  void stop() {
    if (closing) return;
    stop_keepassxc_pairing();
    forget_frames();
    close_native_ui();
    if (output >= 0) command("Browser.close", "{}", "");
    closing = true; fail_downloads(); fail("Brave is shutting down");
    if (integration_poll) { g_source_remove(integration_poll); integration_poll = 0; }
    if (extensions_poll) { g_source_remove(extensions_poll); extensions_poll = 0; }
    if (watch) { g_source_remove(watch); watch = 0; }
    if (input >= 0) ::close(input);
    if (output >= 0) ::close(output);
    input = output = -1;
    g_clear_object(&browser);
    if (server) soup_server_disconnect(server);
  }
};
#include "find.inc"
#include "close-guard.inc"

void initialize(const std::string& profile) { profile_root = profile; signal(SIGPIPE, SIG_IGN); }
void set_automation_mode(bool enabled) { automation_mode = enabled; }
void shutdown() {
  extensions_changed = {};
  for (auto& [mode, process] : processes) process->stop();
  for (auto& [mode, process] : processes) process->drain_private_shutdown();
  auto retired = retiring_private_processes;
  for (auto& process : retired) process->drain_private_shutdown();
  retiring_private_processes.clear();
  processes.clear();
}

void update_preferences(const std::string& json) {
  const bool previous = vim_mode;
  g_autoptr(JsonParser) parser = json_parser_new();
  if (json_parser_load_from_data(parser, json.c_str(), json.size(), nullptr) && JSON_NODE_HOLDS_OBJECT(json_parser_get_root(parser)))
    vim_mode = json_object_get_boolean_member_with_default(json_node_get_object(json_parser_get_root(parser)), "vimMode", false);
  native_preferences = json;
  if (previous != vim_mode)
    for (auto& [mode, process] : processes)
      for (auto& [token, weak] : process->pages) if (auto page = weak.lock()) page->update_preferences();
  for (auto& [mode, process] : processes) if (!process->closing) process->apply_preferences();
}
void set_user_content(const std::string& css, const std::string& script) {
  user_stylesheet = css; user_javascript = script;
  for (auto& [mode, process] : processes)
    for (auto& [token, weak] : process->pages) if (auto page = weak.lock()) page->update_user_content();
}
void browser_command(bool private_profile, const std::string& method, const std::string& params, Reply reply) {
  auto found = processes.find(private_profile);
  if (found == processes.end() || found->second->closing || !found->second->ready) {
    reply(nullptr, "The Brave session is not ready"); return;
  }
  auto process = found->second;
  std::string session;
  if (method.starts_with("Network.")) {
    for (const auto& [token, weak] : process->pages) if (auto page = weak.lock(); page && !page->session.empty() && !page->is_closed()) { session = page->session; break; }
    if (session.empty()) { reply(nullptr, "There is no active Brave page"); return; }
  }
  process->command(method, params, session, std::move(reply));
}
void clear_website_data(bool private_profile, const std::string& types, Reply reply) {
  auto found = processes.find(private_profile);
  if (found == processes.end() || !found->second->integration_ready) { reply(nullptr, "The Brave session is not ready"); return; }
  auto process = found->second;
  process->command("Runtime.evaluate", "{\"expression\":" + quote("gnosisBrowserSettings.clearData(" + types + ")") +
    ",\"awaitPromise\":true,\"returnByValue\":true}", process->integration_session,
    [reply = std::move(reply)](JsonObject* result, const std::string& error) {
      auto* exception = object(result, "exceptionDetails");
      const auto detail = std::string(text(object(exception, "exception"), "description"));
      reply(result, !error.empty() ? error : detail);
    });
}
void initialize_extensions(std::function<void()> changed) {
  extensions_changed = std::move(changed);
}
bool extensions_load(const std::string& path, Reply reply) {
  auto found = processes.find(false);
  if (found == processes.end() || found->second->closing || !found->second->extensions_ready) return false;
  found->second->command("Extensions.loadUnpacked", "{\"path\":" + quote(path) + "}", "", std::move(reply));
  return true;
}
bool extensions_call(const std::string& method, const std::string& arguments, Reply reply) {
  auto found = processes.find(false);
  if (found == processes.end() || found->second->closing || !found->second->extensions_ready) return false;
  if (method != "inventory" && method != "setEnabled") { reply(nullptr, "Unsupported extension operation"); return true; }
  auto process = found->second;
  process->extension_operation(method, arguments, [process, reply = std::move(reply)](JsonObject* result, const std::string& error) {
    // Inventory authority is chrome.management. Disk metadata supplies only
    // manifest details/icons, never installed/enabled state or permissions.
    if (result) {
      json_object_set_string_member(result, "profile", process->profile.c_str());
    }
    reply(result, error);
  });
  return true;
}
Page::Page(GtkWidget* widget, bool ephemeral) : state_(std::make_unique<State>(widget, ephemeral)) {
  auto* manager = adw_style_manager_get_default();
  unsigned index = 0;
  for (const char* signal : {"notify::accent-color", "notify::dark", "notify::high-contrast"})
    state_->theme_signals[index++] = g_signal_connect(manager, signal, G_CALLBACK(+[](AdwStyleManager*, GParamSpec*, gpointer data) {
      static_cast<Page*>(data)->update_preferences();
    }), this);
  if (composited()) return;
  auto* keys = gtk_event_controller_key_new();
  gtk_event_controller_set_propagation_phase(keys, GTK_PHASE_BUBBLE);
  g_signal_connect(keys, "key-pressed", G_CALLBACK(+[](GtkEventControllerKey* controller, guint value, guint, GdkModifierType modifiers, Page* self) -> gboolean {
    if (self->state_->vim_context) if (const char* command = gnosis_compositor_vim_key(value, modifiers)) { self->vim_key(command); return TRUE; }
    if (gnosis_compositor_is_host_shortcut(value, modifiers)) return FALSE;
    self->send_key(gtk_event_controller_get_current_event(GTK_EVENT_CONTROLLER(controller)));
    // Let GTK's window shortcuts continue through their normal propagation path.
    return !(modifiers & (GDK_CONTROL_MASK | GDK_ALT_MASK | GDK_SUPER_MASK));
  }), this);
  g_signal_connect(keys, "key-released", G_CALLBACK(+[](GtkEventControllerKey* controller, guint value, guint, GdkModifierType modifiers, Page* self) {
    if (self->state_->vim_context && gnosis_compositor_vim_key(value, modifiers)) return;
    if (!gnosis_compositor_is_host_shortcut(value, modifiers))
      self->send_key(gtk_event_controller_get_current_event(GTK_EVENT_CONTROLLER(controller)));
  }), this);
  gtk_widget_add_controller(widget, keys);
  g_object_set_data(G_OBJECT(keys), "gnosis-page", this);
  state_->keys = keys;
}
std::shared_ptr<Page> Page::create(GtkWidget* widget, bool ephemeral) { return std::shared_ptr<Page>(new Page(widget, ephemeral)); }
Page::~Page() { close(); }
GtkWidget* Page::widget() const { return state_->content; }
bool Page::is_closed() const { return state_->disposed; }
void Page::set_extension_popup(bool enabled) { state_->extension_popup = enabled; }
void Page::start() {
  if (state_->started || state_->disposed) return;
  auto* root = gtk_widget_get_root(state_->content); if (!root || !GTK_IS_NATIVE(root)) return;
  if (!composited()) {
    auto* surface = gtk_native_get_surface(GTK_NATIVE(root)); if (!GDK_IS_X11_SURFACE(surface)) { g_warning("Native Brave views require X11/XWayland"); return; }
    state_->parent = gdk_x11_surface_get_xid(surface); state_->display = XOpenDisplay(nullptr);
    if (!state_->display) return;
    XSetErrorHandler(ignore_x_error);
  }
  auto& process = processes[state_->ephemeral]; if (!process || process->closing) process = std::make_shared<Process>(state_->ephemeral);
  state_->process = process; state_->started = true;
  process->open(shared_from_this());
  if (!composited()) state_->timer = g_timeout_add(50, +[](gpointer data) -> gboolean { static_cast<Page*>(data)->layout(); return G_SOURCE_CONTINUE; }, this);
}
void Page::adopt_surface(GtkWidget* widget) {
  state_->surface = widget;
  auto* focus = gtk_event_controller_focus_new();
  g_signal_connect(focus, "enter", G_CALLBACK(+[](GtkEventControllerFocus*, gpointer data) {
    static_cast<Page*>(data)->update_portal_parent();
  }), this);
  gtk_widget_add_controller(widget, focus);
  g_signal_connect(widget, "map", G_CALLBACK(+[](GtkWidget*, gpointer data) {
    static_cast<Page*>(data)->update_portal_parent();
  }), this);
  g_signal_connect(widget, "vim-command", G_CALLBACK(+[](GtkWidget*, const char* command, gpointer data) { static_cast<Page*>(data)->vim_key(command); }), this);
  g_signal_connect(widget, "surface-ready", G_CALLBACK(+[](GtkWidget*, gpointer data) { static_cast<Page*>(data)->surface_ready(); }), this);
  g_signal_connect(widget, "fullscreen-changed", G_CALLBACK(+[](GtkWidget*, gboolean value, gpointer data) {
    auto* page = static_cast<Page*>(data); if (page->fullscreen_changed) page->fullscreen_changed(value);
  }), this);
  g_signal_connect(widget, "drag-preview-image", G_CALLBACK(+[](GtkWidget*, GdkDrag* drag, const char* url, gpointer data) {
    auto* page = static_cast<Page*>(data);
    const std::string image = url ? url : "";
    // Read an image already belonging to this page's resource cache. A custom
    // drag payload cannot turn its preview into a filesystem or network fetch.
    if (page->is_closed() || image.size() > 65536 ||
        (!image.starts_with("http://") && !image.starts_with("https://") &&
         !image.starts_with("data:image/") && !image.starts_with("blob:"))) return;
    auto* token = G_CANCELLABLE(g_object_get_data(G_OBJECT(drag), "gnosis-drag-preview-cancel"));
    if (!token) return;
    auto cancel = std::shared_ptr<GCancellable>(G_CANCELLABLE(g_object_ref(token)), g_object_unref);
    auto owner = std::shared_ptr<GdkDrag>(GDK_DRAG(g_object_ref(drag)), g_object_unref);
    const auto expected = page->uri;
    page->command("Page.getResourceContent", "{\"frameId\":" + quote(page->state_->main_frame) + ",\"url\":" + quote(image) + "}",
      [weak = page->weak_from_this(), expected, owner, cancel](JsonObject* result, const std::string& error) {
        auto page = weak.lock();
        if (!page || page->is_closed() || page->uri != expected || !result || !error.empty() || g_cancellable_is_cancelled(cancel.get())) return;
        const char* source = text(result, "content");
        if (strlen(source) > 12 * 1024 * 1024) return;
        gsize length = 0;
        GBytes* bytes;
        if (json_object_get_boolean_member_with_default(result, "base64Encoded", false)) {
          auto* decoded = g_base64_decode(source, &length); bytes = g_bytes_new_take(decoded, length);
        } else bytes = g_bytes_new(source, strlen(source));
        auto* input = g_memory_input_stream_new_from_bytes(bytes); g_bytes_unref(bytes);
        using Decoded = std::function<void(GdkPixbuf*)>;
        auto* done = new Decoded([owner, cancel](GdkPixbuf* pixbuf) {
          if (pixbuf && !g_cancellable_is_cancelled(cancel.get())) {
            auto* texture = gdk_texture_new_for_pixbuf(pixbuf);
            gnosis_compositor_drag_set_image(owner.get(), texture); g_object_unref(texture);
          }
        });
        gdk_pixbuf_new_from_stream_at_scale_async(input, 320, 320, true, cancel.get(), +[](GObject*, GAsyncResult* result, gpointer data) {
          std::unique_ptr<Decoded> done(static_cast<Decoded*>(data));
          g_autoptr(GError) error = nullptr;
          auto* pixbuf = gdk_pixbuf_new_from_stream_finish(result, &error);
          (*done)(pixbuf); g_clear_object(&pixbuf);
        }, done);
        g_object_unref(input);
      });
  }), this);
  gtk_box_append(GTK_BOX(state_->content), widget);
  gtk_widget_set_visible(widget, state_->mapped);
}
void Page::surface_ready() {
  auto& s = *state_;
  if (s.disposed || s.attached || !s.protocol_ready || !s.process->integration_ready || s.process->debug_startup_pending || session.empty() || !s.surface ||
      !gnosis_compositor_view_has_surface(GNOSIS_COMPOSITOR_VIEW(s.surface))) return;
  s.attached = true; navigate(s.requested);
  g_message("Brave composited tab ready: target=%s", target.c_str());
}
void Page::command(const std::string& method, const std::string& params, Reply reply) {
  if (state_->disposed) { if (reply) reply(nullptr, "The tab is closed"); return; }
  if (!state_->process || session.empty()) { if (reply) reply(nullptr, "The Brave tab is not ready"); return; }
  state_->process->command(method, params, session, std::move(reply));
}
void Page::command_for_frame(const std::string& frame,const std::string& method,const std::string& params,Reply reply) {
  auto& s=*state_;
  if (s.disposed || !s.process || s.process->closing) { if(reply)reply(nullptr,"The tab is closed"); return; }
  auto owner=s.process->frame_owners.find(frame);
  if (owner==s.process->frame_owners.end() || owner->second.lock().get()!=this) {
    if(reply)reply(nullptr,"The frame no longer belongs to this tab"); return;
  }
  auto source=s.process->frame_sources.find(frame);
  const auto& owner_session = source == s.process->frame_sources.end() ? session : source->second;
  if (method == "CSS.createStyleSheet" || method == "CSS.setStyleSheetText")
    s.process->style_command(method, params, owner_session, std::move(reply));
  else s.process->command(method, params, owner_session, std::move(reply));
}
namespace {
std::string webpage_theme(GtkWidget* widget) {
  auto* manager = adw_style_manager_get_default();
  g_autoptr(GdkRGBA) accent = adw_style_manager_get_accent_color_rgba(manager);
  GdkRGBA background = *accent, foreground{1, 1, 1, 1};
  auto* context = gtk_widget_get_style_context(widget);
  gtk_style_context_lookup_color(context, "accent_bg_color", &background);
  gtk_style_context_lookup_color(context, "accent_fg_color", &foreground);
  g_autofree char* bg = gdk_rgba_to_string(&background);
  g_autofree char* fg = gdk_rgba_to_string(&foreground);
  GdkRGBA control_fg;
  gtk_widget_get_color(widget, &control_fg);
  gtk_style_context_lookup_color(context, "view_fg_color", &control_fg);
  GdkRGBA popover_bg = adw_style_manager_get_dark(manager) ? GdkRGBA{.212f,.212f,.227f,1} : GdkRGBA{1,1,1,1};
  GdkRGBA popover_fg = control_fg, view_bg = popover_bg;
  gtk_style_context_lookup_color(context, "popover_bg_color", &popover_bg);
  gtk_style_context_lookup_color(context, "popover_fg_color", &popover_fg);
  gtk_style_context_lookup_color(context, "view_bg_color", &view_bg);
  g_autofree char* pop_bg = gdk_rgba_to_string(&popover_bg);
  g_autofree char* pop_fg = gdk_rgba_to_string(&popover_fg);
  g_autofree char* view = gdk_rgba_to_string(&view_bg);
  g_autofree char* font_name = nullptr;
  g_object_get(gtk_widget_get_settings(widget), "gtk-font-name", &font_name, nullptr);
  auto* font = pango_font_description_from_string(font_name ? font_name : "Adwaita Sans 11");
  const char* family = pango_font_description_get_family(font);
  const auto font_family = quote(family ? family : "Adwaita Sans");
  double font_size = pango_font_description_get_size(font) / double(PANGO_SCALE);
  if (!pango_font_description_get_size_is_absolute(font)) font_size *= 96.0 / 72.0;
  pango_font_description_free(font);
  const std::string overlay = ":root{--gnosis-kpxc-bg:" + std::string(pop_bg) + "!important;--gnosis-kpxc-fg:" + pop_fg +
    "!important;--gnosis-kpxc-view-bg:" + view + "!important;--gnosis-kpxc-accent:" + bg +
    "!important;--gnosis-kpxc-accent-fg:" + fg + "!important;--gnosis-kpxc-font:" + font_family +
    "!important;--gnosis-kpxc-font-size:" + std::to_string(font_size > 0 ? font_size : 14.667) + "px!important;}";
  return overlay + "*::selection{background-color:" + std::string(bg) + "!important;color:" + fg + "!important;}";
}
}
void Page::theme_document(const std::string& frame, const std::string& loader) {
  auto& s = *state_;
  if (s.disposed || frame.empty()) return;
  auto& style = s.theme_frames[frame];
  if (style.loader != loader) {
    style = {}; style.loader = loader; style.generation = ++s.theme_generation;
  }
  if (!s.theme_ready || style.pending) return;
  if (!style.sheet.empty()) return;
  style.pending = true;
  const auto generation = style.generation;
  const auto expected_session = session;
  command_for_frame(frame,"CSS.createStyleSheet", "{\"frameId\":" + quote(frame) + ",\"force\":true}",
    [weak = weak_from_this(), frame, generation, expected_session](JsonObject* result, const std::string& error) {
      auto page = weak.lock();
      if (!page || page->is_closed() || page->session != expected_session) return;
      auto found = page->state_->theme_frames.find(frame);
      if (found == page->state_->theme_frames.end() || found->second.generation != generation) return;
      found->second.pending = false;
      if (!result || !error.empty()) return; // Detached/OOPIF/privileged frames may reject the request.
      found->second.sheet = text(result, "styleSheetId");
      if (!found->second.sheet.empty()) page->command_for_frame(frame,"CSS.setStyleSheetText", "{\"styleSheetId\":" + quote(found->second.sheet) +
        ",\"text\":" + quote(webpage_theme(page->widget()) +
          (adapter_enabled("popup-sizing") && page->state_->extension_popup && page->uri.starts_with("chrome-extension://") ? popup_layout : "") +
          ((page->uri.starts_with("https://") || page->uri.starts_with("http://") || page->uri.starts_with("file://")) ? (adapter_enabled("user-content") ? user_stylesheet : "") : "")) + "}");
    });
}
void Page::update_web_theme() {
  auto& s = *state_;
  if (s.disposed || !s.theme_ready) return;
  command("Emulation.setEmulatedMedia", web_color_scheme());
  for (const auto& [source, frame] : s.process->frame_sessions)
    if (frame.owner.lock().get() == this)
      s.process->command("Emulation.setEmulatedMedia", web_color_scheme(), source);
  const auto css = webpage_theme(s.content) + (adapter_enabled("popup-sizing") && s.extension_popup && uri.starts_with("chrome-extension://") ? popup_layout : "") +
    ((uri.starts_with("https://") || uri.starts_with("http://") || uri.starts_with("file://")) ? (adapter_enabled("user-content") ? user_stylesheet : "") : "");
  for (const auto& [frame, style] : s.theme_frames) {
    if (style.sheet.empty()) theme_document(frame, style.loader);
    else command_for_frame(frame,"CSS.setStyleSheetText", "{\"styleSheetId\":" + quote(style.sheet) + ",\"text\":" + quote(css) + "}");
  }
}
void Page::attach() {
  if (state_->process->native_fd >= 0) {
    const auto message = "{\"kind\":\"authenticator-owner\",\"target\":" + quote(target) + ",\"enabled\":true}";
    if (send(state_->process->native_fd, message.data(), message.size(), MSG_NOSIGNAL | MSG_DONTWAIT) != static_cast<ssize_t>(message.size()))
      state_->process->close_native_ui();
  }
  update_portal_parent();
  update_user_content();
  state_->process->attach_frames(session);
  command("Page.enable"); command("Runtime.enable"); command("Network.enable"); command("Security.enable");
  state_->protocol_ready = false;
  state_->theme_ready = true; state_->theme_frames.clear(); ++state_->theme_generation;
  update_web_theme();
  command("Runtime.addBinding", "{\"name\":\"__gnosisContextMenu\",\"executionContextName\":\"GnosisContextMenu\"}");
  g_autofree char* menu_path = g_build_filename(GNOSIS_NATIVE_INTEGRATION_DIR, "context-menu.js", nullptr);
  g_autofree char* menu_script = nullptr;
  if (adapter_enabled("context-menu") && g_file_get_contents(menu_path, &menu_script, nullptr, nullptr))
    command("Page.addScriptToEvaluateOnNewDocument", "{\"worldName\":\"GnosisContextMenu\",\"source\":" + quote(menu_script) + "}");
  else if (adapter_enabled("context-menu")) g_warning("The native context-menu adapter could not be loaded");
  command("Runtime.addBinding", "{\"name\":\"__gnosisDocumentMetadata\",\"executionContextName\":\"GnosisDocumentMetadata\"}");
  if (adapter_enabled("document-metadata")) command("Page.addScriptToEvaluateOnNewDocument", "{\"worldName\":\"GnosisDocumentMetadata\",\"source\":" + quote(document_adapter_source()) + "}");
  if (state_->extension_popup && adapter_enabled("popup-sizing")) {
    g_autofree char* popup_path = g_build_filename(GNOSIS_NATIVE_INTEGRATION_DIR, "popup-size.js", nullptr);
    g_autofree char* popup_script = nullptr;
    if (g_file_get_contents(popup_path, &popup_script, nullptr, nullptr))
      command("Page.addScriptToEvaluateOnNewDocument", "{\"worldName\":\"GnosisDocumentMetadata\",\"source\":" + quote(popup_script) + "}");
  }
  command("Runtime.addBinding", "{\"name\":\"__gnosisNativeMessage\"}");
  const std::string script = "(()=>{const send=globalThis.__gnosisNativeMessage;delete globalThis.__gnosisNativeMessage;if(location.origin!==" + quote(state_->internal_origin) + R"JS(||window!==top)return;
    const events=new EventTarget();
    Object.defineProperty(globalThis,'Gnosis',{value:Object.freeze({
      postMessage:(name,payload)=>send(JSON.stringify({name,payload})),
      addEventListener:(...args)=>events.addEventListener(...args),
      removeEventListener:(...args)=>events.removeEventListener(...args)
    })});
    Object.defineProperty(globalThis,'__gnosisDispatch',{value:(name,payload)=>events.dispatchEvent(new CustomEvent(name,{detail:payload}))});
  })())JS";
  command("Page.addScriptToEvaluateOnNewDocument", "{\"source\":" + quote(script) + "}");
  command("Fetch.enable", "{\"patterns\":[{\"urlPattern\":" + quote(state_->internal_origin + "/*") + ",\"requestStage\":\"Request\"}]}");
  auto weak = weak_from_this();
  command("Page.getFrameTree", "{}", [weak](JsonObject* result, const std::string&) {
    auto self = weak.lock();
    auto* frame = object(object(result, "frameTree"), "frame");
    if (self && frame) {
      self->state_->main_frame = text(frame, "id");
      auto page = self;
      std::function<void(JsonObject*)> register_frames = [&](JsonObject* tree) {
        auto* item = object(tree, "frame");
        if (item) {
          page->state_->process->frame_owners[text(item, "id")] = page;
          page->theme_document(text(item, "id"), text(item, "loaderId"));
        }
        auto* children = tree && json_object_has_member(tree, "childFrames") ? json_object_get_array_member(tree, "childFrames") : nullptr;
        for (guint i = 0; children && i < json_array_get_length(children); ++i) register_frames(json_array_get_object_element(children, i));
      };
      register_frames(object(result, "frameTree"));
      self->state_->protocol_ready = true;
      if (composited()) self->surface_ready();
    }
  });
  state_->process->command("Browser.getWindowForTarget", "{\"targetId\":" + quote(target) + "}", "", [weak](JsonObject* result, const std::string&) {
    if (auto self = weak.lock(); self && result) {
      self->state_->window = json_object_get_int_member(result, "windowId"); self->apply_zoom();
    }
  });
}
namespace {
// Read a favicon through this page's actual Brave network session, including
// its cookies, proxy, certificate checks and extension filtering. Never use
// a separate Soup session or a third-party favicon service.
struct ResourceRead : std::enable_shared_from_this<ResourceRead> {
  std::shared_ptr<Process> process;
  std::string session, stream, bytes;
  size_t limit = 2 * 1024 * 1024;
  std::function<void(const std::string&)> complete;
  void finish(bool okay) {
    process->command("IO.close", "{\"handle\":" + quote(stream) + "}", session);
    if (okay) complete(bytes);
  }
  void next() {
    if (process->closing) return;
    process->command("IO.read", "{\"handle\":" + quote(stream) + ",\"size\":65536}", session,
      [self=shared_from_this()](JsonObject*result,const std::string&error) {
        if (!result || !error.empty()) { self->finish(false); return; }
        const char* data=text(result,"data");
        if (json_object_get_boolean_member_with_default(result,"base64Encoded",false)) {
          gsize length=0;g_autofree guchar*decoded=g_base64_decode(data,&length);
          self->bytes.append(reinterpret_cast<const char*>(decoded),length);
        } else self->bytes+=data;
        if (self->bytes.size()>self->limit) { self->finish(false); return; }
        if (json_object_get_boolean_member_with_default(result,"eof",false)) self->finish(true);
        else self->next();
      });
  }
};
}
void Page::request_favicon() {
  if (is_closed() || !state_->attached || state_->main_frame.empty() || uri == state_->bootstrap) return;
  const auto document=uri; const auto generation=state_->navigation_generation;
  if ((document.starts_with("https://") || document.starts_with("http://")) &&
      (state_->favicon_document != document || state_->favicon_generation != generation)) {
    state_->favicon_document = document; state_->favicon_uri.clear();
    if (favicon_changed) favicon_changed(document, "", nullptr);
  }
  command("Runtime.evaluate", "{\"expression\":\"({url:location.href,title:document.title,icon:document.querySelector('link[rel~=icon]')?.href || (['http:','https:'].includes(location.protocol)?new URL('/favicon.ico',location.href).href:'' )})\",\"returnByValue\":true}",
    [weak=weak_from_this(),document,generation](JsonObject*result,const std::string&error) {
      auto page=weak.lock(); if (!page || page->is_closed() || page->uri!=document || generation!=page->state_->navigation_generation || !error.empty()) return;
      auto* value=object(object(result,"result"),"value");
      if (!value || page->canonical_uri(text(value,"url")) != document) return;
      page->state_->document_title_uri=document;
      const std::string document_title=text(value,"title");
      if (page->title != document_title) { page->title=document_title; if (page->changed) page->changed(); }
      if (!document.starts_with("https://") && !document.starts_with("http://")) return;
      const std::string icon=text(value,"icon");
      if ((!icon.starts_with("https://") && !icon.starts_with("http://") && !icon.starts_with("data:image/")) || icon.size()>256*1024) return;
      if (page->state_->favicon_generation==generation && page->state_->favicon_uri==icon) return;
      page->state_->favicon_generation=generation;page->state_->favicon_uri=icon;
      const auto process=page->state_->process;const auto session=page->session;
      page->command("Network.loadNetworkResource", "{\"frameId\":"+quote(page->state_->main_frame)+",\"url\":"+quote(icon)+",\"options\":{\"disableCache\":false,\"includeCredentials\":true}}",
        [weak,document,icon,generation,process,session](JsonObject*result,const std::string&error) {
          auto*resource=object(result,"resource");const std::string stream=text(resource,"stream");
          if (!error.empty() || !resource || stream.empty()) return;
          if (!json_object_get_boolean_member_with_default(resource,"success",false)) {process->command("IO.close","{\"handle\":"+quote(stream)+"}",session);return;}
          auto read=std::make_shared<ResourceRead>();read->process=process;read->session=session;read->stream=stream;
          read->complete=[weak,document,icon,generation](const std::string&data) {
            auto page=weak.lock();if (!page || page->is_closed() || page->uri!=document || generation!=page->state_->navigation_generation || data.empty()) return;
            auto*input=g_memory_input_stream_new_from_data(data.data(),data.size(),nullptr);
            g_autoptr(GError)error=nullptr;
            auto*pixbuf=gdk_pixbuf_new_from_stream_at_scale(input,64,64,true,nullptr,&error);g_object_unref(input);
            if (!pixbuf) return;
            auto*texture=gdk_texture_new_for_pixbuf(pixbuf);g_object_unref(pixbuf);
            if (page->favicon_changed) page->favicon_changed(document,icon,texture);
            g_object_unref(texture);
          };
          read->next();
        });
    });
}
void Page::dismiss_http_retry() {
  auto* dialog = std::exchange(state_->http_retry, nullptr);
  if (!dialog) return;
  g_signal_handlers_disconnect_by_data(dialog, this);
  adw_dialog_force_close(dialog); g_object_unref(dialog);
}
void Page::offer_http_retry(const std::string& address) {
  auto* root = gtk_widget_get_root(widget());
  if (is_closed() || state_->http_retry || !state_->mapped || !GTK_IS_WINDOW(root) ||
      !address.starts_with("https://") || !gnosis_uri_is_loopback(address.c_str())) return;
  auto* dialog = adw_alert_dialog_new(_("Use HTTP for Local Site?"),
    _("This local server does not appear to support HTTPS. Open it without encryption?"));
  state_->http_retry = ADW_DIALOG(g_object_ref_sink(dialog));
  adw_alert_dialog_add_responses(ADW_ALERT_DIALOG(dialog), "cancel", _("_Cancel"), "http", _("Use _HTTP"), nullptr);
  adw_alert_dialog_set_default_response(ADW_ALERT_DIALOG(dialog), "http");
  adw_alert_dialog_set_close_response(ADW_ALERT_DIALOG(dialog), "cancel");
  struct Retry { std::weak_ptr<Page> page; std::string address; guint64 generation; };
  adw_alert_dialog_choose(ADW_ALERT_DIALOG(dialog), GTK_WIDGET(root), nullptr,
    +[](GObject* source, GAsyncResult* result, gpointer data) {
      std::unique_ptr<Retry> retry(static_cast<Retry*>(data));
      const char* response = adw_alert_dialog_choose_finish(ADW_ALERT_DIALOG(source), result);
      auto page = retry->page.lock();
      if (!page || page->state_->http_retry != ADW_DIALOG(source)) return;
      page->state_->http_retry = nullptr;
      g_object_unref(source);
      if (!strcmp(response, "http") && !page->is_closed() && page->state_->mapped &&
          page->state_->navigation_generation == retry->generation) page->navigate(retry->address);
    }, new Retry{weak_from_this(), "http://" + address.substr(8), state_->navigation_generation});
}
void Page::navigate(const std::string& value) {
  dismiss_http_retry();
  dismiss_passkey_provider();
  dismiss_authenticator(true);
  dismiss_context_menu();
  dismiss_script_dialog(true);
  dismiss_install_dialog(true);
  dismiss_translation();
  dismiss_vim_commands();
  set_vim_context(0);
  dismiss_password_manager();
  state_->requested = value;
  state_->authorized_document = served_document(value) ? value : "";
  state_->fulfilled_document.clear(); state_->committed_document.clear();
  state_->context_id = 0; state_->context_unique.clear(); ++state_->navigation_generation;
  state_->response_status = 0;
  if (!state_->attached) { uri = value; if (changed) changed(); start(); return; }
  command("Page.navigate", "{\"url\":" + quote(transport_uri(state_->internal_origin, value)) + "}");
}
std::string Page::canonical_uri(const std::string& value) const {
  for (const char* scheme : {"ephy-about", "ephy-resource", "ephy-reader"}) {
    std::string prefix = state_->internal_origin + '/' + scheme + '/';
    if (value.starts_with(prefix)) return std::string(scheme) + ':' + value.substr(prefix.size());
  }
  return value;
}
bool Page::is_internal_page(const std::string& value) const {
  return !state_->disposed && internal_document(value) && uri == value && state_->committed_document == value;
}
void Page::internal_request(JsonObject* params) {
  const std::string id = text(params, "requestId");
  auto* request = object(params, "request");
  const std::string value = canonical_uri(text(request, "url"));
  const bool main = std::string(text(params, "resourceType")) == "Document";
  const bool trusted = main ? (value == state_->authorized_document || !state_->committed_document.empty()) : !state_->committed_document.empty();
  if (!trusted || std::string(text(params, "frameId")) != state_->main_frame ||
      std::string(text(request, "method")) != "GET" || !internal_resource ||
      (main ? !served_document(value) : !(value.starts_with("ephy-resource:") || served_document(value)))) {
    if (g_getenv("GNOSIS_TRACE_INTERNAL")) g_message("Native resource denied: main=%d trusted=%d frame-match=%d handler=%d", main, trusted,
      std::string(text(params, "frameId")) == state_->main_frame, bool(internal_resource));
    command("Fetch.failRequest", "{\"requestId\":" + quote(id) + ",\"errorReason\":\"AccessDenied\"}"); return;
  }
  auto weak = weak_from_this();
  const std::string frame = state_->main_frame;
  const guint64 generation = state_->navigation_generation;
  internal_resource(value, main, [weak, id, frame, generation, value, main](GBytes* bytes, const std::string& mime, const std::string& error) {
    auto self = weak.lock(); if (!self || self->is_closed()) return;
    if (!bytes || !error.empty() || frame != self->state_->main_frame || generation != self->state_->navigation_generation ||
        (main ? !(value == self->state_->authorized_document || !self->state_->committed_document.empty()) : self->state_->committed_document.empty())) {
      if (!error.empty()) g_warning("Native internal resource failed: %s", error.c_str());
      self->command("Fetch.failRequest", "{\"requestId\":" + quote(id) + ",\"errorReason\":\"AccessDenied\"}"); return;
    }
    gsize length = 0; const char* data = static_cast<const char*>(g_bytes_get_data(bytes, &length));
    std::string content(data, length);
    if (mime.starts_with("text/") || mime.find("javascript") != std::string::npos || mime == "image/svg+xml")
      content = rewrite_resources(std::move(content), self->state_->internal_origin);
    g_autofree char* encoded = g_base64_encode(reinterpret_cast<const guchar*>(content.data()), content.size());
    if (main) self->state_->fulfilled_document = value;
    if (g_getenv("GNOSIS_TRACE_INTERNAL"))
      g_message("Native resource fulfilled: main=%d mime=%s bytes=%zu", main, mime.c_str(), content.size());
    self->command("Fetch.fulfillRequest", "{\"requestId\":" + quote(id) + ",\"responseCode\":200,\"responseHeaders\":["
      "{\"name\":\"Content-Type\",\"value\":" + quote(mime) + "},"
      "{\"name\":\"Cache-Control\",\"value\":\"no-store\"},"
      "{\"name\":\"Referrer-Policy\",\"value\":\"no-referrer\"},"
      "{\"name\":\"X-Frame-Options\",\"value\":\"DENY\"},"
      "{\"name\":\"X-Content-Type-Options\",\"value\":\"nosniff\"}],\"body\":" + quote(encoded) + "}");
  });
}
bool Page::send_page_message(const std::string& expected, const std::string& name, const std::string& json) {
  if (!is_internal_page(expected) || state_->context_unique.empty() || name.size() > 128 || json.size() > 1048576) return false;
  command("Runtime.evaluate", "{\"uniqueContextId\":" + quote(state_->context_unique) + ",\"expression\":" +
    quote("globalThis.__gnosisDispatch?.(" + quote(name) + "," + rewrite_resources(json, state_->internal_origin) + ")") + "}");
  return true;
}
void Page::refresh_history() {
  auto weak = weak_from_this();
  command("Page.getNavigationHistory", "{}", [weak](JsonObject* result, const std::string&) {
    auto self = weak.lock(); if (!self || !result) return;
    auto* entries = json_object_get_array_member(result, "entries"); int index = json_object_get_int_member(result, "currentIndex");
    self->can_back = index > 0; self->can_forward = entries && index + 1 < int(json_array_get_length(entries));
    if (self->changed) self->changed();
  });
}
void Page::history(int delta) {
  auto weak = weak_from_this(); command("Page.getNavigationHistory", "{}", [weak, delta](JsonObject* result, const std::string&) {
    auto self = weak.lock(); if (!self || !result) return;
    auto* entries = json_object_get_array_member(result, "entries"); int index = json_object_get_int_member(result, "currentIndex") + delta;
    if (entries && index >= 0 && index < int(json_array_get_length(entries))) {
      auto* item = json_array_get_object_element(entries, index);
      self->command("Page.navigateToHistoryEntry", "{\"entryId\":" + std::to_string(json_object_get_int_member(item, "id")) + "}");
    }
  });
}
void Page::back() { history(-1); } void Page::forward() { history(1); }
void Page::reload(bool bypass) { command("Page.reload", bypass ? "{\"ignoreCache\":true}" : "{}"); }
void Page::stop() { command("Page.stopLoading"); }
void Page::handle(const std::string& method, JsonObject* params, bool frame_event) {
  if (state_->disposed || !params) return;
  if (method == "Page.frameAttached") {
    if (state_->close_approved || state_->close_callback) cancel_close();
    state_->process->frame_owners[text(params, "frameId")] = shared_from_this();
  }
  else if (method == "Page.frameDetached" && std::string(text(params, "reason")) == "remove") {
    if (state_->close_approved || state_->close_callback) cancel_close();
    state_->process->frame_owners.erase(text(params, "frameId"));
    state_->theme_frames.erase(text(params, "frameId"));
  }
  else if (method == "Page.frameNavigated") {
    auto* frame = object(params, "frame");
    if (frame) {
      if (std::string(text(frame,"id")) != state_->main_frame && (state_->close_approved || state_->close_callback)) cancel_close();
      state_->process->frame_owners[text(frame, "id")] = shared_from_this();
      theme_document(text(frame, "id"), text(frame, "loaderId"));
    }
  }
  if (g_getenv("GNOSIS_TRACE_LOADS") && (method == "Page.frameStartedLoading" || method == "Page.frameStoppedLoading" ||
      method == "Page.frameNavigated" || method == "Page.navigatedWithinDocument" || method == "Page.loadEventFired")) {
    auto* frame = object(params, "frame");
    g_message("Brave load trace: %s frame=%s main=%s loading=%d", method.c_str(), frame ? text(frame,"id") : text(params,"frameId"),
      state_->main_frame.c_str(), loading);
  }
  if (method == "CSS.styleSheetRemoved") {
    for (auto& [frame, style] : state_->theme_frames)
      if (style.sheet == text(params, "styleSheetId")) style.sheet.clear();
  } else if (method == "Page.javascriptDialogOpening") {
    show_script_dialog(params);
  } else if (method == "Page.javascriptDialogClosed") {
    dismiss_script_dialog(false);
  } else if (method == "Fetch.requestPaused") {
    internal_request(params);
  } else if (method == "Runtime.executionContextsCleared") {
    set_vim_context(0); dismiss_vim_commands();
    dismiss_context_menu(); state_->menu_contexts.clear(); state_->menu_frames.clear(); state_->menu_sessions.clear(); state_->metadata_contexts.clear(); state_->metadata_unique.clear();
    state_->context_id = 0; state_->context_unique.clear();
  } else if (method == "Runtime.executionContextDestroyed") {
    state_->menu_contexts.erase(json_object_get_int_member_with_default(params, "executionContextId", 0));
    state_->menu_frames.erase(json_object_get_int_member_with_default(params, "executionContextId", 0));
    state_->menu_sessions.erase(json_object_get_int_member_with_default(params, "executionContextId", 0));
    if (state_->vim_context == json_object_get_int_member_with_default(params, "executionContextId", 0)) set_vim_context(0);
    state_->metadata_contexts.erase(json_object_get_int_member_with_default(params, "executionContextId", 0));
    state_->metadata_unique.erase(json_object_get_int_member_with_default(params, "executionContextId", 0));
    if (json_object_get_int_member_with_default(params, "executionContextId", 0) == state_->context_id) {
      state_->context_id = 0; state_->context_unique.clear();
    }
  } else if (method == "Runtime.executionContextCreated") {
    auto* context = object(params, "context"); auto* aux = object(context, "auxData");
    if (context && aux && std::string(text(context, "name")) == "GnosisContextMenu" &&
        !std::string(text(context, "uniqueId")).empty()) {
      state_->menu_contexts[json_object_get_int_member(context, "id")] = text(context, "uniqueId");
      state_->menu_frames[json_object_get_int_member(context, "id")] = text(aux, "frameId");
      if (!frame_event) state_->menu_sessions[json_object_get_int_member(context, "id")] = session;
    }
    if (context && aux && std::string(text(context, "name")) == "GnosisDocumentMetadata" && *text(context, "uniqueId")) {
      const int id = json_object_get_int_member(context, "id");
      state_->metadata_contexts[id] = text(aux, "frameId");
      state_->metadata_unique[id] = text(context, "uniqueId");
      if (!frame_event) state_->menu_sessions[id] = session;
    }
    if (context && aux && std::string(text(context, "origin")) == state_->internal_origin &&
        std::string(text(aux, "frameId")) == state_->main_frame && json_object_get_boolean_member_with_default(aux, "isDefault", false)) {
      state_->context_id = json_object_get_int_member(context, "id"); state_->context_unique = text(context, "uniqueId");
    }
  } else if (method == "Runtime.bindingCalled") {
    const std::string payload = text(params, "payload");
    auto metadata_context = state_->metadata_contexts.find(json_object_get_int_member_with_default(params, "executionContextId", 0));
    if (std::string(text(params,"name")) == "__gnosisDocumentMetadata" && payload.size() <= 256*1024 &&
        metadata_context != state_->metadata_contexts.end()) {
      g_autoptr(JsonParser) parser = json_parser_new();
      if (json_parser_load_from_data(parser,payload.data(),payload.size(),nullptr) && JSON_NODE_HOLDS_OBJECT(json_parser_get_root(parser))) {
        auto* value = json_node_get_object(json_parser_get_root(parser));
        if (std::string(text(value, "kind")) == "activity") {
          if (state_->close_approved || state_->close_callback) cancel_close();
          return;
        }
        if (std::string(text(value, "kind")) == "vim-edit") {
          const std::string operation = text(value, "operation");
          if (vim_mode && state_->vim_context == metadata_context->first && (operation == "Copy" || operation == "Paste")) edit(operation);
          return;
        }
        if (std::string(text(value, "kind")) == "vim-blur") {
          if (state_->vim_context == metadata_context->first && !state_->vim_commands) set_vim_context(0);
          return;
        }
        if (std::string(text(value, "kind")) == "vim-focus") {
          const bool active = json_object_get_boolean_member_with_default(value, "active", false);
          set_vim_context(active && vim_mode && !state_->extension_popup ? metadata_context->first : 0);
          return;
        }
        if (std::string(text(value, "kind")) == "vim-field") {
          const bool active = json_object_get_boolean_member_with_default(value, "active", false);
          if (active && vim_mode && !state_->extension_popup) set_vim_context(metadata_context->first);
          else if (!active && state_->vim_context == metadata_context->first) set_vim_context(0);
          return;
        }
        if (std::string(text(value, "kind")) == "vim-ready") {
          update_vim(metadata_context->first);
          return;
        }
        if (std::string(text(value, "kind")) == "vim-action") {
          if (!vim_mode || state_->extension_popup || is_closed()) return;
          auto* root = gtk_widget_get_root(widget());
          auto* focus = root ? gtk_root_get_focus(root) : nullptr;
          if (!focus || (focus != widget() && !gtk_widget_is_ancestor(focus, widget()))) return;
          const std::string action = text(value, "action");
          if (action == "vim.commands") { show_vim_commands(); return; }
          static const std::set<std::string> allowed = {
            "win.vim-find", "win.find-next", "win.find-prev", "win.location", "win.new-tab",
            "win.vim-tab-next", "win.vim-tab-previous", "toolbar.reload",
            "toolbar.navigation-back", "toolbar.navigation-forward", "tab.close"};
          if (allowed.contains(action)) {
            gtk_widget_activate_action(widget(), action.c_str(), nullptr);
            // Vim n/N must leave focus in the page so repeated navigation
            // does not start typing into the reopened search entry.
            if (action == "win.find-next" || action == "win.find-prev") this->focus();
          }
          return;
        }
        if (metadata_context->second != state_->main_frame) return;
        if (state_->extension_popup && uri.starts_with("chrome-extension://") &&
            canonical_uri(text(value, "url")) == uri &&
            std::string(text(value, "kind")) == "popup-size") {
          const double width = json_object_get_double_member_with_default(value, "width", 0);
          const double height = json_object_get_double_member_with_default(value, "height", 0);
          if (std::isfinite(width) && std::isfinite(height) && width > 0 && height > 0 &&
              width <= 16384 && height <= 16384) {
            const int w = std::clamp(int(std::ceil(width)), 25, 800);
            const int h = std::clamp(int(std::ceil(height)), 25, 600);
            if (w != state_->popup_width || h != state_->popup_height) {
              state_->popup_width = w; state_->popup_height = h;
              if (popup_size_changed) popup_size_changed(w, h);
            }
          }
          return;
        }
        auto* name = json_object_get_member(value,"title");
        if (canonical_uri(text(value,"url")) == uri && name && JSON_NODE_HOLDS_VALUE(name) && json_node_get_value_type(name) == G_TYPE_STRING) {
          state_->document_title_uri = uri; title = json_node_get_string(name);
          if (changed) changed();
          request_favicon();
        }
      }
    } else if (std::string(text(params, "name")) == "__gnosisContextMenu" && payload.size() <= 131072 &&
        state_->menu_contexts.contains(json_object_get_int_member_with_default(params, "executionContextId", 0))) {
      g_autoptr(JsonParser) parser = json_parser_new();
      if (json_parser_load_from_data(parser, payload.data(), payload.size(), nullptr) && JSON_NODE_HOLDS_OBJECT(json_parser_get_root(parser)))
        show_context_menu(json_node_get_object(json_parser_get_root(parser)), json_object_get_int_member(params,"executionContextId"));
      return;
    }
    if (std::string(text(params, "name")) == "__gnosisNativeMessage" && is_internal_page(uri) &&
        state_->context_id != 0 && json_object_get_int_member_with_default(params, "executionContextId", 0) == state_->context_id &&
        !state_->context_unique.empty() && payload.size() <= 1048576 && page_message) {
      g_autoptr(JsonParser) parser = json_parser_new();
      if (json_parser_load_from_data(parser, payload.data(), payload.size(), nullptr) && JSON_NODE_HOLDS_OBJECT(json_parser_get_root(parser))) {
        auto* message = json_node_get_object(json_parser_get_root(parser));
        const std::string name = text(message, "name");
        if (!name.empty() && name.size() <= 128 && json_object_has_member(message, "payload")) {
          g_autofree char* json = json_to_string(json_object_get_member(message, "payload"), false);
          if (g_getenv("GNOSIS_TRACE_INTERNAL")) g_message("Native page message: %s", name.c_str());
          page_message(uri, name, json);
        }
      }
    }
  } else if (method == "Security.visibleSecurityStateChanged") {
    auto* security = object(params, "visibleSecurityState");
    auto* certificate = object(security, "certificateSecurityState");
    state_->security_state = text(security, "securityState");
    g_clear_object(&state_->certificate); state_->tls_errors = GTlsCertificateFlags(0);
    if (certificate) {
      auto* chain = json_object_get_array_member(certificate, "certificate");
      std::string pem;
      for (guint i = 0; chain && i < json_array_get_length(chain); ++i) {
        const char* encoded = json_array_get_string_element(chain, i); if (!encoded) continue;
        std::string der(encoded); pem += "-----BEGIN CERTIFICATE-----\n";
        for (size_t offset = 0; offset < der.size(); offset += 64) pem += der.substr(offset, 64) + '\n';
        pem += "-----END CERTIFICATE-----\n";
      }
      if (!pem.empty()) state_->certificate = g_tls_certificate_new_from_pem(pem.c_str(), pem.size(), nullptr);
      const std::string error = text(certificate, "certificateNetworkError");
      unsigned flags = 0;
      if (error.find("COMMON_NAME_INVALID") != std::string::npos) flags |= G_TLS_CERTIFICATE_BAD_IDENTITY;
      else if (error.find("AUTHORITY_INVALID") != std::string::npos) flags |= G_TLS_CERTIFICATE_UNKNOWN_CA;
      else if (error.find("REVOKED") != std::string::npos) flags |= G_TLS_CERTIFICATE_REVOKED;
      else if (error.find("DATE_INVALID") != std::string::npos) {
        const double now = double(g_get_real_time()) / G_USEC_PER_SEC;
        if (json_object_get_double_member_with_default(certificate, "validFrom", 0) > now) flags |= G_TLS_CERTIFICATE_NOT_ACTIVATED;
        if (json_object_get_double_member_with_default(certificate, "validTo", now) < now) flags |= G_TLS_CERTIFICATE_EXPIRED;
        if (!flags) flags |= G_TLS_CERTIFICATE_GENERIC_ERROR;
      } else if (!error.empty()) flags |= G_TLS_CERTIFICATE_GENERIC_ERROR;
      if (json_object_get_boolean_member_with_default(certificate, "certificateHasWeakSignature", false)) flags |= G_TLS_CERTIFICATE_INSECURE;
      state_->tls_errors = GTlsCertificateFlags(flags);
    }
    if (tls_changed) tls_changed();
  } else if (method == "Network.requestWillBeSent" && state_->attached &&
             std::string(text(params, "type")) == "Document" &&
             std::string(text(params, "frameId")) == state_->main_frame) {
    state_->document_request = text(params, "requestId");
    state_->document_request_url = text(object(params, "request"), "url");
  } else if (method == "Network.loadingFailed" && state_->attached &&
             std::string(text(params, "requestId")) == state_->document_request &&
             std::string(text(params, "errorText")) == "net::ERR_SSL_PROTOCOL_ERROR") {
    offer_http_retry(state_->document_request_url);
  } else if (method == "Page.frameStartedLoading" && state_->attached &&
             std::string(text(params, "frameId")) == state_->main_frame) {
    loading = true; progress = 0.1; state_->response_status = 0;
    if (changed) changed();
  } else if (method == "Page.frameNavigated") {
    auto* frame = object(params, "frame");
    if (frame && !json_object_has_member(frame, "parentId")) {
      // The bootstrap's commit can be delivered after attaching the mapped
      // surface and requesting the native document. It is not that navigation.
      if (std::string(text(frame, "url")) == state_->bootstrap) {
        state_->main_frame = text(frame, "id");
        if (event) event(method, params);
        return;
      }
      stop_find();
      cancel_close();
      dismiss_translation();
      dismiss_password_manager();
      dismiss_authenticator(true);
      dismiss_passkey_provider();
      g_clear_object(&state_->certificate); state_->security_state.clear(); state_->tls_errors = GTlsCertificateFlags(0);
      state_->main_frame = text(frame, "id");
      state_->context_id = 0; state_->context_unique.clear();
      if (state_->attached) {
        uri = canonical_uri(std::string(text(frame, "url")) + text(frame, "urlFragment")); state_->requested = uri; progress = loading ? 0.1 : 1;
        const std::string loader = text(frame, "loaderId");
        const bool restored = state_->internal_loaders.contains(loader) && state_->internal_loaders[loader] == uri;
        state_->committed_document = served_document(uri) && (uri == state_->fulfilled_document || restored) ? uri : "";
        if (!state_->committed_document.empty() && !loader.empty()) state_->internal_loaders[loader] = uri;
        if (!state_->committed_document.empty()) state_->authorized_document.clear();
        if (navigation_started) navigation_started(uri);
        if (changed) changed();
        // A cached history commit can arrive after frameStoppedLoading. Do
        // not restart its spinner, and finish the newly created shell resource.
        if (!loading) {
          refresh_history();
          if (load_finished) load_finished(state_->response_status);
        }
        state_->zoom_pending = true; apply_zoom();
      }
    }
  } else if (method == "Network.responseReceived" && state_->attached &&
             std::string(text(params, "type")) == "Document" &&
             std::string(text(params, "frameId")) == state_->main_frame) {
    auto* response = object(params, "response");
    if (response) state_->response_status = json_object_get_int_member(response, "status");
  } else if (method == "Page.navigatedWithinDocument" && state_->attached &&
             std::string(text(params, "frameId")) == state_->main_frame) {
    uri = canonical_uri(text(params, "url")); refresh_history(); if (changed) changed();
    request_favicon();
  } else if (method == "Page.frameStoppedLoading" && state_->attached &&
             std::string(text(params, "frameId")) == state_->main_frame) {
    // DOM load events do not run again for back/forward-cache restores, and
    // aborted navigations may never dispatch one. Track the frame's actual
    // loading lifetime instead; child frames must not finish the main page.
    bool was_loading = loading;
    loading = false; progress = 1; refresh_history(); if (changed) changed();
    if (was_loading && load_finished) load_finished(state_->response_status);
    request_favicon();
    apply_zoom();
    if (is_internal_page(uri) && g_getenv("GNOSIS_TRACE_INTERNAL")) {
      command("Runtime.evaluate", "{\"returnByValue\":true,\"expression\":\"({overview:!!document.getElementById('overview'),stylesheets:document.styleSheets.length,bridge:typeof Gnosis,ready:document.readyState})\"}",
        [](JsonObject* result, const std::string& error) {
          if (!error.empty()) { g_warning("Native document inspection failed: %s", error.c_str()); return; }
          auto* value = object(result, "result");
          if (value && json_object_has_member(value, "value")) {
            g_autofree char* json = json_to_string(json_object_get_member(value, "value"), false);
            g_message("Native document inspection: %s", json);
          }
        });
    }
  }
  if (!frame_event && event) event(method, params);
}
namespace {
std::string window_title(Display* display, ::Window window) {
  Atom type; int format; unsigned long count, remaining; unsigned char* data = nullptr;
  std::string result;
  if (XGetWindowProperty(display, window, XInternAtom(display, "_NET_WM_NAME", false), 0, 4096, false, AnyPropertyType,
      &type, &format, &count, &remaining, &data) == Success && data) { if (format == 8) result.assign(reinterpret_cast<char*>(data), count); XFree(data); }
  return result;
}
::Window find_window(Display* display, ::Window root, const std::string& window_class,
                     const std::string& token, unsigned depth = 0) {
  if (depth > 5) return 0;
  XClassHint hint{};
  bool matches = false;
  if (XGetClassHint(display, root, &hint)) {
    // Mutter's decoration frame copies the client's title. Only Brave's own
    // WM_CLASS identifies the client; reparenting the decoration instead leaves
    // a managed ghost window and the page clipped to the old frame's bounds.
    matches = hint.res_class && window_class == hint.res_class &&
      ((hint.res_name && std::string(hint.res_name).find(token) != std::string::npos) ||
       window_title(display, root).find("GnosisTab-" + token) != std::string::npos);
    if (hint.res_class) XFree(hint.res_class);
    if (hint.res_name) XFree(hint.res_name);
  }
  bool normal = false;
  if (matches) {
    Atom type; int format; unsigned long count, remaining; unsigned char* values = nullptr;
    if (XGetWindowProperty(display, root, XInternAtom(display, "_NET_WM_WINDOW_TYPE", false),
        0, 16, false, XA_ATOM, &type, &format, &count, &remaining, &values) == Success && values) {
      if (format == 32) for (unsigned long i = 0; i < count; ++i)
        normal |= reinterpret_cast<Atom*>(values)[i] == XInternAtom(display, "_NET_WM_WINDOW_TYPE_NORMAL", false);
      XFree(values);
    }
  }
  XWindowAttributes attributes{};
  if (matches && normal && XGetWindowAttributes(display, root, &attributes) &&
      attributes.c_class == InputOutput && attributes.map_state == IsViewable &&
      attributes.width > 200 && attributes.height > 150) return root;
  ::Window actual_root, parent, *children = nullptr; unsigned count = 0; ::Window found = 0;
  if (XQueryTree(display, root, &actual_root, &parent, &children, &count)) {
    for (unsigned i = 0; i < count && !found; ++i) found = find_window(display, children[i], window_class, token, depth + 1);
    if (children) XFree(children);
  }
  return found;
}
}
void Page::layout() {
  auto& s = *state_; if (s.disposed || !s.display) return;
  if (!s.process || !s.process->integration_ready) return;
  auto* root_widget = gtk_widget_get_root(s.content); if (!root_widget || !GTK_IS_NATIVE(root_widget)) return;
  auto* surface = gtk_native_get_surface(GTK_NATIVE(root_widget)); if (!GDK_IS_X11_SURFACE(surface)) return;
  ::Window parent = gdk_x11_surface_get_xid(surface);
  if (s.parent != parent) { s.parent = parent; if (s.container) XReparentWindow(s.display, s.container, parent, 0, 0); }
  if (!s.child) {
    s.child = find_window(s.display, DefaultRootWindow(s.display), s.process->window_class, s.token);
    if (!s.child) return;
    XWithdrawWindow(s.display, s.child, DefaultScreen(s.display)); XUnmapWindow(s.display, s.child); XSync(s.display, false);
    s.withdrawn_until = g_get_monotonic_time() + 300000; return;
  }
  if (!s.container) {
    if (g_get_monotonic_time() < s.withdrawn_until || session.empty()) return;
    XSetWindowAttributes attributes{}; attributes.override_redirect = True; XChangeWindowAttributes(s.display, s.child, CWOverrideRedirect, &attributes);
    s.container = XCreateSimpleWindow(s.display, s.parent, 0, 0, 1, 1, 0, 0, 0);
    XReparentWindow(s.display, s.child, s.container, 0, 0); XSetWindowBorderWidth(s.display, s.child, 0);
    unsigned long extents[] = {0, 0, 0, 0};
    for (auto* name : {"_GTK_FRAME_EXTENTS", "_NET_FRAME_EXTENTS"})
      XChangeProperty(s.display, s.child, XInternAtom(s.display, name, false), XA_CARDINAL, 32, PropModeReplace, reinterpret_cast<unsigned char*>(extents), 4);
    struct { unsigned long flags, functions, decorations; long input; unsigned long status; } hints{2, 0, 0, 0, 0};
    XChangeProperty(s.display, s.child, XInternAtom(s.display, "_MOTIF_WM_HINTS", false), XInternAtom(s.display, "_MOTIF_WM_HINTS", false), 32, PropModeReplace, reinterpret_cast<unsigned char*>(&hints), 5);
    Atom states[] = {XInternAtom(s.display, "_NET_WM_STATE_SKIP_TASKBAR", false), XInternAtom(s.display, "_NET_WM_STATE_SKIP_PAGER", false)};
    XChangeProperty(s.display, s.child, XInternAtom(s.display, "_NET_WM_STATE", false), XA_ATOM, 32, PropModeReplace, reinterpret_cast<unsigned char*>(states), 2);
    unsigned long embed_info[] = {0, 1};
    XChangeProperty(s.display, s.child, XInternAtom(s.display, "_XEMBED_INFO", false), XInternAtom(s.display, "_XEMBED_INFO", false), 32, PropModeReplace, reinterpret_cast<unsigned char*>(embed_info), 2);
    unsigned long withdrawn[] = {WithdrawnState, None};
    XChangeProperty(s.display, s.child, XInternAtom(s.display, "WM_STATE", false), XInternAtom(s.display, "WM_STATE", false), 32, PropModeReplace, reinterpret_cast<unsigned char*>(withdrawn), 2);
    XSetTransientForHint(s.display, s.child, s.parent); XMapWindow(s.display, s.child);
    // A synchronous passive grab performs click-to-focus, then replays the actual
    // pointer event to Brave. It does not emulate mouse buttons or keyboard text.
    XGrabButton(s.display, Button1, AnyModifier, s.child, false, ButtonPressMask, GrabModeSync, GrabModeAsync, None, None);
    s.attached = true; navigate(s.requested);
    g_message("Brave native tab embedded: target=%s parent=%lu container=%lu child=%lu", target.c_str(), s.parent, s.container, s.child);
  }
  const bool mapped = s.mapped && gtk_widget_get_mapped(s.content);
  if (!mapped) { XUnmapWindow(s.display, s.container); XFlush(s.display); return; }
  XMapWindow(s.display, s.container);
  ::Window actual_root, actual_parent, *children = nullptr; unsigned count = 0;
  if (XQueryTree(s.display, s.child, &actual_root, &actual_parent, &children, &count)) {
    if (children) XFree(children);
    if (actual_parent != s.container) {
      XSetWindowAttributes attributes{}; attributes.override_redirect = True;
      XUnmapWindow(s.display, s.child);
      XChangeWindowAttributes(s.display, s.child, CWOverrideRedirect, &attributes);
      XReparentWindow(s.display, s.child, s.container, 0, 0); XMapWindow(s.display, s.child);
    }
  }
  graphene_point_t zero{0, 0}, point{}; if (!gtk_widget_compute_point(s.content, GTK_WIDGET(root_widget), &zero, &point)) return;
  double x, y; gtk_native_get_surface_transform(GTK_NATIVE(root_widget), &x, &y);
  int scale = gtk_widget_get_scale_factor(s.content), width = gtk_widget_get_width(s.content) * scale, height = gtk_widget_get_height(s.content) * scale;
  if (width > 0 && height > 0) {
    XMoveResizeWindow(s.display, s.container, (point.x + x) * scale, (point.y + y) * scale, width, height);
    if (s.window >= 0 && (s.width != width || s.height != height)) {
      s.process->command("Browser.setWindowBounds", "{\"windowId\":" + std::to_string(s.window) + ",\"bounds\":{\"left\":0,\"top\":0,\"width\":" + std::to_string(width) + ",\"height\":" + std::to_string(height) + "}}", "");
      s.width = width; s.height = height;
    }
    XMoveResizeWindow(s.display, s.child, 0, 0, width, height); XRaiseWindow(s.display, s.container);
  }
  while (XPending(s.display)) {
    XEvent event; XNextEvent(s.display, &event);
    if (event.type == ButtonPress) { focus(); XAllowEvents(s.display, ReplayPointer, event.xbutton.time); }
  }
  XFlush(s.display);
}
bool Page::edit(const std::string& name) {
  auto& s = *state_;
  return s.surface && GNOSIS_IS_COMPOSITOR_VIEW(s.surface) &&
    gnosis_compositor_view_edit(GNOSIS_COMPOSITOR_VIEW(s.surface), name.c_str());
}
void Page::update_portal_parent() {
  auto& s = *state_;
  if (s.disposed || !s.process || s.process->native_fd < 0 || target.empty() || !s.mapped) return;
  auto* root = gtk_widget_get_root(s.content);
  if (!GTK_IS_WINDOW(root)) return;
  auto* surface = gtk_native_get_surface(GTK_NATIVE(root));
  if (!surface) return;
  s.process->portal_target = target;
  std::string parent;
  if (GDK_IS_WAYLAND_TOPLEVEL(surface)) {
    struct Export {
      GdkToplevel* surface;
      std::string handle;
      std::vector<std::weak_ptr<Page>> waiting;
      Export(GdkToplevel* value, std::weak_ptr<Page> page) : surface(GDK_TOPLEVEL(g_object_ref(value))), waiting{page} {}
      ~Export() {
        if (!handle.empty() && !gdk_surface_is_destroyed(GDK_SURFACE(surface)))
          gdk_wayland_toplevel_drop_exported_handle(surface, handle.c_str());
        g_object_unref(surface);
      }
    };
    auto* cached = static_cast<std::shared_ptr<Export>*>(g_object_get_data(G_OBJECT(root), "gnosis-portal-parent"));
    if (cached && (*cached)->surface != GDK_TOPLEVEL(surface)) {
      g_object_set_data(G_OBJECT(root), "gnosis-portal-parent", nullptr); cached = nullptr;
    }
    if (!cached) {
      auto exported = std::make_shared<Export>(GDK_TOPLEVEL(surface), weak_from_this());
      g_object_set_data_full(G_OBJECT(root), "gnosis-portal-parent", new std::shared_ptr<Export>(exported),
        +[](gpointer data) { delete static_cast<std::shared_ptr<Export>*>(data); });
      auto* pending = new std::shared_ptr<Export>(exported);
      if (!gdk_wayland_toplevel_export_handle(exported->surface,
          +[](GdkToplevel*, const char* handle, gpointer data) {
            auto exported = *static_cast<std::shared_ptr<Export>*>(data); exported->handle = handle ? handle : "";
            if (exported->handle.empty()) { exported->waiting.clear(); return; }
            auto waiting = std::move(exported->waiting);
            for (auto& weak : waiting) if (auto page = weak.lock(); page && page->state_->process &&
                page->state_->process->portal_target == page->target) page->update_portal_parent();
          }, pending, +[](gpointer data) { delete static_cast<std::shared_ptr<Export>*>(data); })) {
        delete pending;
        g_object_set_data(G_OBJECT(root), "gnosis-portal-parent", nullptr);
      }
      return;
    }
    const auto& exported = *cached;
    if (exported->handle.empty()) {
      exported->waiting.push_back(weak_from_this()); return;
    }
    parent = "wayland:" + exported->handle;
  } else if (GDK_IS_X11_SURFACE(surface)) {
    char xid[32]; g_snprintf(xid, sizeof xid, "x11:%lx", gdk_x11_surface_get_xid(surface)); parent = xid;
  }
  const auto message = "{\"kind\":\"parent-window\",\"handle\":" + quote(parent) + "}";
  if (send(s.process->native_fd, message.data(), message.size(), MSG_NOSIGNAL | MSG_DONTWAIT) != static_cast<ssize_t>(message.size()))
    s.process->close_native_ui();
}
void Page::focus() {
  auto& s = *state_;
  update_portal_parent();
  if (s.surface) { gtk_widget_grab_focus(s.surface); return; }
  if (!s.display || !s.child || s.disposed) return;
  gtk_widget_grab_focus(s.content);
  // The WM only knows the GTK toplevel. Giving server focus to the foreign
  // child makes Mutter deactivate GTK. Retain native focus in the host and
  // deliver its real keyboard events to the embedded client instead.
  XEvent active{}; active.xclient.type = ClientMessage; active.xclient.window = s.parent;
  active.xclient.message_type = XInternAtom(s.display, "_NET_ACTIVE_WINDOW", false);
  active.xclient.format = 32; active.xclient.data.l[0] = 1;
  XSendEvent(s.display, DefaultRootWindow(s.display), false, SubstructureRedirectMask | SubstructureNotifyMask, &active);
  XSetInputFocus(s.display, s.parent, RevertToParent, CurrentTime);
  XEvent event{}; event.xfocus.type = FocusIn; event.xfocus.window = s.child;
  event.xfocus.mode = NotifyNormal; event.xfocus.detail = NotifyAncestor;
  XSendEvent(s.display, s.child, false, FocusChangeMask, &event); XFlush(s.display);
}
void Page::send_key(GdkEvent* source) {
  auto& s = *state_; if (!source || !s.display || !s.child || s.disposed) return;
  auto type = gdk_event_get_event_type(source);
  if (type != GDK_KEY_PRESS && type != GDK_KEY_RELEASE) return;
  XEvent event{}; event.xkey.type = type == GDK_KEY_PRESS ? KeyPress : KeyRelease;
  event.xkey.display = s.display; event.xkey.window = s.child;
  event.xkey.root = DefaultRootWindow(s.display); event.xkey.same_screen = True;
  event.xkey.time = gdk_event_get_time(source);
  event.xkey.keycode = gdk_key_event_get_keycode(source);
  auto modifiers = gdk_event_get_modifier_state(source);
  if (modifiers & GDK_SHIFT_MASK) event.xkey.state |= ShiftMask;
  if (modifiers & GDK_LOCK_MASK) event.xkey.state |= LockMask;
  if (modifiers & GDK_CONTROL_MASK) event.xkey.state |= ControlMask;
  if (modifiers & GDK_ALT_MASK) event.xkey.state |= Mod1Mask;
  if (modifiers & GDK_SUPER_MASK) event.xkey.state |= Mod4Mask;
  XSendEvent(s.display, s.child, false, type == GDK_KEY_PRESS ? KeyPressMask : KeyReleaseMask, &event);
  XFlush(s.display);
}
void Page::dismiss_context_menu() {
  if (state_->context_menu) gtk_popover_popdown(GTK_POPOVER(state_->context_menu));
}
void Page::dismiss_install_dialog(bool cancel) {
  auto& s = *state_;
  const gint64 id = std::exchange(s.install_id, 0);
  if (s.install_delay) { g_source_remove(s.install_delay); s.install_delay = 0; }
  if (s.install_dialog) {
    auto* dialog = std::exchange(s.install_dialog, nullptr);
    if (s.install_response) g_signal_handler_disconnect(dialog, s.install_response);
    s.install_response = 0;
    if (auto* list = g_object_get_data(G_OBJECT(dialog), "chooser-list")) {
      g_signal_handlers_disconnect_by_data(list, dialog);
      g_object_set_data(G_OBJECT(dialog), "chooser-list", nullptr);
    }
    adw_dialog_force_close(dialog);
    g_object_unref(dialog);
  }
  if (cancel && id && s.process) s.process->native_decide(id, false);
}
static bool chooser_has_selection(AdwAlertDialog* dialog) {
  auto* list = GTK_LIST_BOX(g_object_get_data(G_OBJECT(dialog), "chooser-list"));
  if (!list) return false;
  GList* selected = gtk_list_box_get_selected_rows(list);
  const bool any = selected != nullptr;
  g_list_free(selected);
  return any;
}
static void populate_device_chooser(AdwAlertDialog* dialog, JsonObject* message) {
  auto* options = json_object_get_array_member(message, "options");
  auto* list = gtk_list_box_new();
  gtk_list_box_set_selection_mode(GTK_LIST_BOX(list), json_object_get_boolean_member(message, "multiple") ? GTK_SELECTION_MULTIPLE : GTK_SELECTION_SINGLE);
  gtk_widget_add_css_class(list, "boxed-list");
  for (guint i = 0; i < json_array_get_length(options); ++i) {
    auto* label = gtk_label_new(json_array_get_string_element(options, i));
    gtk_label_set_wrap(GTK_LABEL(label), true);
    gtk_label_set_xalign(GTK_LABEL(label), 0);
    gtk_widget_set_margin_start(label, 12); gtk_widget_set_margin_end(label, 12);
    gtk_widget_set_margin_top(label, 12); gtk_widget_set_margin_bottom(label, 12);
    gtk_list_box_append(GTK_LIST_BOX(list), label);
  }
  const bool enabled = json_object_get_boolean_member(message, "enabled");
  auto* empty = gtk_label_new(enabled ? _("No devices found") : _("The device adapter is unavailable"));
  gtk_widget_set_margin_top(empty, 24); gtk_widget_set_margin_bottom(empty, 24);
  gtk_list_box_set_placeholder(GTK_LIST_BOX(list), empty);
  gtk_widget_set_sensitive(list, enabled);
  auto* scroll = gtk_scrolled_window_new();
  gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(scroll), GTK_POLICY_NEVER, GTK_POLICY_AUTOMATIC);
  gtk_scrolled_window_set_min_content_height(GTK_SCROLLED_WINDOW(scroll), 160);
  gtk_scrolled_window_set_max_content_height(GTK_SCROLLED_WINDOW(scroll), 320);
  gtk_scrolled_window_set_propagate_natural_height(GTK_SCROLLED_WINDOW(scroll), true);
  gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(scroll), list);
  if (auto* previous = g_object_get_data(G_OBJECT(dialog), "chooser-list"))
    g_signal_handlers_disconnect_by_data(previous, dialog);
  g_object_set_data(G_OBJECT(dialog), "chooser-list", list);
  g_object_set_data_full(G_OBJECT(dialog), "chooser-revision", g_strdup_printf("%" G_GINT64_FORMAT, json_object_get_int_member(message, "revision")), g_free);
  adw_alert_dialog_set_response_enabled(dialog, "accept", false);
  adw_alert_dialog_set_extra_child(dialog, scroll);
  g_signal_connect_object(list, "selected-rows-changed", G_CALLBACK(+[](GtkListBox* list, gpointer data) {
    auto* dialog = ADW_ALERT_DIALOG(data);
    if (g_object_get_data(G_OBJECT(dialog), "chooser-list") != list) return;
    adw_alert_dialog_set_response_enabled(dialog, "accept", g_object_get_data(G_OBJECT(dialog), "chooser-ready") && chooser_has_selection(dialog));
  }), dialog, G_CONNECT_DEFAULT);
}
void Page::show_install_dialog(JsonObject* message) {
  auto& s = *state_;
  const bool chooser = !strcmp(text(message, "kind"), "chooser");
  const bool uninstall = !strcmp(text(message, "kind"), "uninstall");
  if (chooser && s.install_dialog && s.install_id == json_object_get_int_member(message, "id")) {
    populate_device_chooser(ADW_ALERT_DIALOG(s.install_dialog), message); return;
  }
  dismiss_install_dialog(true);
  dismiss_translation();
  dismiss_context_menu();
  s.install_id = json_object_get_int_member(message, "id");
  auto* root = gtk_widget_get_root(s.content);
  if (!s.mapped || !root || !GTK_IS_WINDOW(root)) { dismiss_install_dialog(true); return; }
  if (s.surface && GNOSIS_IS_COMPOSITOR_VIEW(s.surface))
    gnosis_compositor_view_release_buttons(GNOSIS_COMPOSITOR_VIEW(s.surface));
  // Use the native model's localized title and actual permission warnings.
  // Plain labels prevent extension names or warning text injecting markup.
  const bool permission = !strcmp(text(message, "kind"), "permission");
  const bool once = permission && json_object_get_boolean_member(message, "once");
  auto* dialog = uninstall ? adw_alert_dialog_new(text(message, "title"), nullptr) :
    adw_alert_dialog_new(!strcmp(text(message, "kind"), "file-access") ? _("File Access") : chooser ? _("Connect a Device") : permission ? _("Website Permission") : _("Extension Confirmation"), text(message, "title"));
  s.install_dialog = ADW_DIALOG(g_object_ref_sink(dialog));
  adw_alert_dialog_add_response(ADW_ALERT_DIALOG(dialog), "cancel", text(message, "cancel"));
  if (permission) adw_alert_dialog_add_response(ADW_ALERT_DIALOG(dialog), "block", _("Block"));
  if (once) {
    adw_alert_dialog_add_response(ADW_ALERT_DIALOG(dialog), "once", _("Allow Only This Time"));
    adw_alert_dialog_set_response_appearance(ADW_ALERT_DIALOG(dialog), "once", ADW_RESPONSE_SUGGESTED);
    adw_alert_dialog_set_response_enabled(ADW_ALERT_DIALOG(dialog), "once", false);
  }
  adw_alert_dialog_add_response(ADW_ALERT_DIALOG(dialog), "accept", text(message, "accept"));
  adw_alert_dialog_set_default_response(ADW_ALERT_DIALOG(dialog), "cancel");
  adw_alert_dialog_set_close_response(ADW_ALERT_DIALOG(dialog), "cancel");
  adw_alert_dialog_set_response_appearance(ADW_ALERT_DIALOG(dialog), "accept", uninstall ? ADW_RESPONSE_DESTRUCTIVE : ADW_RESPONSE_SUGGESTED);
  adw_alert_dialog_set_response_enabled(ADW_ALERT_DIALOG(dialog), "accept", false);
  auto* warnings = json_object_get_array_member(message, "permissions");
  if (json_array_get_length(warnings)) {
    auto* box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 12);
    for (guint i = 0; i <= json_array_get_length(warnings); ++i) {
      auto* label = gtk_label_new(i ? json_array_get_string_element(warnings, i - 1) : text(message, "heading"));
      gtk_label_set_wrap(GTK_LABEL(label), true);
      gtk_label_set_xalign(GTK_LABEL(label), 0);
      if (!i) gtk_widget_add_css_class(label, "heading");
      gtk_box_append(GTK_BOX(box), label);
    }
    auto* scroll = gtk_scrolled_window_new();
    gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(scroll), GTK_POLICY_NEVER, GTK_POLICY_AUTOMATIC);
    gtk_scrolled_window_set_max_content_height(GTK_SCROLLED_WINDOW(scroll), 260);
    gtk_scrolled_window_set_propagate_natural_height(GTK_SCROLLED_WINDOW(scroll), true);
    gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(scroll), box);
    adw_alert_dialog_set_extra_child(ADW_ALERT_DIALOG(dialog), scroll);
  }
  if (uninstall && *text(message, "checkbox")) {
    auto* check = gtk_check_button_new_with_label(text(message, "checkbox"));
    gtk_check_button_set_active(GTK_CHECK_BUTTON(check), json_object_get_boolean_member_with_default(message, "checked", false));
    auto* extra = adw_alert_dialog_get_extra_child(ADW_ALERT_DIALOG(dialog));
    if (extra && GTK_IS_SCROLLED_WINDOW(extra))
      gtk_box_append(GTK_BOX(gtk_scrolled_window_get_child(GTK_SCROLLED_WINDOW(extra))), check);
    else adw_alert_dialog_set_extra_child(ADW_ALERT_DIALOG(dialog), check);
    g_object_set_data(G_OBJECT(dialog), "uninstall-report", check);
  }
  if (chooser) populate_device_chooser(ADW_ALERT_DIALOG(dialog), message);
  struct Response { std::weak_ptr<Page> page; gint64 id; };
  auto* response = new Response{weak_from_this(), s.install_id};
  s.install_response = g_signal_connect_data(dialog, "response", G_CALLBACK(+[](AdwAlertDialog* dialog, const char* result, gpointer pointer) {
    auto* response = static_cast<Response*>(pointer);
    if (auto page = response->page.lock(); page && page->state_->install_id == response->id) {
      auto process = page->state_->process;
      const auto id = response->id;
      std::string choice = result;
      const bool accept = choice == "accept" || choice == "once";
      auto* report = static_cast<GtkCheckButton*>(g_object_get_data(G_OBJECT(dialog), "uninstall-report"));
      if (accept && report && gtk_check_button_get_active(report)) choice = "report-abuse";
      if (accept && g_object_get_data(G_OBJECT(dialog), "chooser-list")) {
        choice = "devices:" + std::string(static_cast<const char*>(g_object_get_data(G_OBJECT(dialog), "chooser-revision"))) + ":";
        GList* selected = gtk_list_box_get_selected_rows(GTK_LIST_BOX(g_object_get_data(G_OBJECT(dialog), "chooser-list")));
        for (GList* row = selected; row; row = row->next) {
          if (row != selected) choice += ",";
          choice += std::to_string(gtk_list_box_row_get_index(GTK_LIST_BOX_ROW(row->data)));
        }
        g_list_free(selected);
      }
      page->dismiss_install_dialog(false);
      if (process) process->native_decide(id, accept, choice);
    }
  }), response, +[](gpointer pointer, GClosure*) { delete static_cast<Response*>(pointer); }, G_CONNECT_DEFAULT);
  s.install_delay = g_timeout_add_full(G_PRIORITY_DEFAULT, 500, +[](gpointer pointer) -> gboolean {
    auto* response = static_cast<Response*>(pointer);
    if (auto page = response->page.lock(); page && page->state_->install_id == response->id) {
      page->state_->install_delay = 0;
      auto* dialog = ADW_ALERT_DIALOG(page->state_->install_dialog);
      if (g_object_get_data(G_OBJECT(dialog), "chooser-list")) {
        g_object_set_data(G_OBJECT(dialog), "chooser-ready", GINT_TO_POINTER(1));
        adw_alert_dialog_set_response_enabled(dialog, "accept", chooser_has_selection(dialog));
      } else adw_alert_dialog_set_response_enabled(dialog, "accept", true);
      if (adw_alert_dialog_has_response(dialog, "once")) adw_alert_dialog_set_response_enabled(dialog, "once", true);
    }
    return G_SOURCE_REMOVE;
  }, new Response{weak_from_this(), s.install_id}, +[](gpointer pointer) { delete static_cast<Response*>(pointer); });
  adw_dialog_present(dialog, GTK_WIDGET(root));
}
void Page::dismiss_script_dialog(bool cancel) {
  auto& s = *state_;
  const bool pending = std::exchange(s.script_dialog_open, false);
  ++s.script_dialog_generation;
  if (s.script_dialog) {
    auto* dialog = std::exchange(s.script_dialog, nullptr);
    if (s.script_dialog_response) g_signal_handler_disconnect(dialog, s.script_dialog_response);
    s.script_dialog_response = 0;
    adw_dialog_force_close(dialog);
    g_object_unref(dialog);
  }
  if (cancel && pending && !s.disposed && !s.native_destroyed)
    command("Page.handleJavaScriptDialog", "{\"accept\":false}");
}
void Page::show_script_dialog(JsonObject* params) {
  if (!adapter_enabled("script-dialogs")) {
    state_->script_dialog_open = true;
    if (std::string(text(params,"type")) == "beforeunload" && state_->close_callback) state_->close_prompted = true;
    return;
  }
  auto& s = *state_;
  // This event comes from the inherited protocol session, never a page binding.
  // Background tabs cannot grab another tab's GTK dialog surface.
  dismiss_script_dialog(false);
  s.script_dialog_open = true;
  auto* root = gtk_widget_get_root(s.content);
  const std::string kind = text(params, "type");
  if (kind == "beforeunload" && s.close_callback) s.close_prompted = true;
  const bool explicit_close = kind == "beforeunload" && bool(s.close_callback);
  if ((!s.mapped && !explicit_close) || !root || !GTK_IS_WINDOW(root) ||
      (kind != "alert" && kind != "confirm" && kind != "prompt" && kind != "beforeunload")) {
    dismiss_script_dialog(true); return;
  }
  dismiss_context_menu();
  if (explicit_close) gtk_window_present(GTK_WINDOW(root));
  if (s.surface && GNOSIS_IS_COMPOSITOR_VIEW(s.surface))
    gnosis_compositor_view_release_buttons(GNOSIS_COMPOSITOR_VIEW(s.surface));
  const bool unload = kind == "beforeunload";
  auto* dialog = adw_alert_dialog_new(unload ? _("Leave This Page?") : _("Website Message"),
    unload ? _("Changes you made may not be saved.") : text(params, "message"));
  s.script_dialog = ADW_DIALOG(g_object_ref_sink(dialog));
  if (kind != "alert")
    adw_alert_dialog_add_response(ADW_ALERT_DIALOG(dialog), "cancel", unload ? _("_Keep Open") : _("_Cancel"));
  adw_alert_dialog_add_response(ADW_ALERT_DIALOG(dialog), "accept", unload ? _("_Leave") : _("_OK"));
  adw_alert_dialog_set_default_response(ADW_ALERT_DIALOG(dialog), unload ? "cancel" : "accept");
  adw_alert_dialog_set_close_response(ADW_ALERT_DIALOG(dialog), kind == "alert" ? "accept" : "cancel");
  if (unload) adw_alert_dialog_set_response_appearance(ADW_ALERT_DIALOG(dialog), "accept", ADW_RESPONSE_DESTRUCTIVE);
  // The browser supplies the requesting frame URL; expose its origin as plain
  // text so a cross-origin frame cannot impersonate the top-level site.
  g_autoptr(GUri) origin = g_uri_parse(text(params, "url"), G_URI_FLAGS_PARSE_RELAXED, nullptr);
  GtkWidget* entry = nullptr;
  auto* box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 12);
  const char* host = origin ? g_uri_get_host(origin) : nullptr;
  if (host && *host) {
    g_autofree char* label_text = g_uri_join(G_URI_FLAGS_NONE, g_uri_get_scheme(origin), nullptr,
      host, g_uri_get_port(origin), "", nullptr, nullptr);
    auto* label = gtk_label_new(label_text);
    gtk_label_set_wrap(GTK_LABEL(label), true);
    gtk_widget_add_css_class(label, "dim-label");
    gtk_box_append(GTK_BOX(box), label);
  }
  if (kind == "prompt") {
    entry = gtk_entry_new();
    gtk_entry_set_activates_default(GTK_ENTRY(entry), true);
    gtk_editable_set_text(GTK_EDITABLE(entry), text(params, "defaultPrompt"));
    gtk_accessible_update_property(GTK_ACCESSIBLE(entry), GTK_ACCESSIBLE_PROPERTY_LABEL, _("Response"), -1);
    gtk_box_append(GTK_BOX(box), entry);
  }
  if (gtk_widget_get_first_child(box)) adw_alert_dialog_set_extra_child(ADW_ALERT_DIALOG(dialog), box);
  else { g_object_ref_sink(box); g_object_unref(box); }
  struct Response {
    std::weak_ptr<Page> page;
    guint64 generation;
    GtkWidget* entry;
  };
  auto* response = new Response{weak_from_this(), s.script_dialog_generation, entry};
  s.script_dialog_response = g_signal_connect_data(dialog, "response", G_CALLBACK(+[](AdwAlertDialog*, const char* result, gpointer pointer) {
    auto* response = static_cast<Response*>(pointer);
    auto page = response->page.lock();
    if (!page || page->is_closed() || !page->state_->script_dialog_open ||
        page->state_->script_dialog_generation != response->generation) return;
    const bool accept = !strcmp(result, "accept");
    const std::string input = accept && response->entry ? gtk_editable_get_text(GTK_EDITABLE(response->entry)) : "";
    page->state_->script_dialog_open = false;
    page->command("Page.handleJavaScriptDialog", std::string("{\"accept\":") + (accept ? "true" : "false") +
      ",\"promptText\":" + quote(input) + "}");
    page->dismiss_script_dialog(false);
  }), response, +[](gpointer pointer, GClosure*) { delete static_cast<Response*>(pointer); }, G_CONNECT_DEFAULT);
  adw_dialog_present(dialog, GTK_WIDGET(root));
  if (entry) gtk_widget_grab_focus(entry);
}

namespace {
struct TranslationLanguage { const char *code, *name; };
const TranslationLanguage translation_languages[] = {
  {"af", N_("Afrikaans")}, {"sq", N_("Albanian")}, {"am", N_("Amharic")},
  {"ar", N_("Arabic")}, {"hy", N_("Armenian")}, {"az", N_("Azerbaijani")},
  {"eu", N_("Basque")}, {"be", N_("Belarusian")}, {"bn", N_("Bengali")},
  {"bs", N_("Bosnian")}, {"bg", N_("Bulgarian")}, {"ca", N_("Catalan")},
  {"zh-CN", N_("Chinese (Simplified)")}, {"zh-TW", N_("Chinese (Traditional)")},
  {"hr", N_("Croatian")}, {"cs", N_("Czech")}, {"da", N_("Danish")},
  {"nl", N_("Dutch")}, {"en", N_("English")}, {"eo", N_("Esperanto")},
  {"et", N_("Estonian")}, {"fil", N_("Filipino")}, {"fi", N_("Finnish")},
  {"fr", N_("French")}, {"gl", N_("Galician")}, {"ka", N_("Georgian")},
  {"de", N_("German")}, {"el", N_("Greek")}, {"gu", N_("Gujarati")},
  {"ht", N_("Haitian Creole")}, {"ha", N_("Hausa")}, {"haw", N_("Hawaiian")},
  {"iw", N_("Hebrew")}, {"hi", N_("Hindi")}, {"hu", N_("Hungarian")},
  {"is", N_("Icelandic")}, {"ig", N_("Igbo")}, {"id", N_("Indonesian")},
  {"ga", N_("Irish")}, {"it", N_("Italian")}, {"ja", N_("Japanese")},
  {"jw", N_("Javanese")}, {"kn", N_("Kannada")}, {"kk", N_("Kazakh")},
  {"km", N_("Khmer")}, {"ko", N_("Korean")}, {"ku", N_("Kurdish")},
  {"ky", N_("Kyrgyz")}, {"lo", N_("Lao")}, {"la", N_("Latin")},
  {"lv", N_("Latvian")}, {"lt", N_("Lithuanian")}, {"lb", N_("Luxembourgish")},
  {"mk", N_("Macedonian")}, {"mg", N_("Malagasy")}, {"ms", N_("Malay")},
  {"ml", N_("Malayalam")}, {"mt", N_("Maltese")}, {"mi", N_("Māori")},
  {"mr", N_("Marathi")}, {"mn", N_("Mongolian")}, {"my", N_("Myanmar (Burmese)")},
  {"ne", N_("Nepali")}, {"no", N_("Norwegian")}, {"ps", N_("Pashto")},
  {"fa", N_("Persian")}, {"pl", N_("Polish")}, {"pt", N_("Portuguese")},
  {"pa", N_("Punjabi")}, {"ro", N_("Romanian")}, {"ru", N_("Russian")},
  {"sm", N_("Samoan")}, {"gd", N_("Scots Gaelic")}, {"sr", N_("Serbian")},
  {"st", N_("Sesotho")}, {"sn", N_("Shona")}, {"sd", N_("Sindhi")},
  {"si", N_("Sinhala")}, {"sk", N_("Slovak")}, {"sl", N_("Slovenian")},
  {"so", N_("Somali")}, {"es", N_("Spanish")}, {"su", N_("Sundanese")},
  {"sw", N_("Swahili")}, {"sv", N_("Swedish")}, {"tg", N_("Tajik")},
  {"ta", N_("Tamil")}, {"te", N_("Telugu")}, {"th", N_("Thai")},
  {"tr", N_("Turkish")}, {"uk", N_("Ukrainian")}, {"ur", N_("Urdu")},
  {"uz", N_("Uzbek")}, {"vi", N_("Vietnamese")}, {"cy", N_("Welsh")},
  {"xh", N_("Xhosa")}, {"yi", N_("Yiddish")}, {"yo", N_("Yoruba")}, {"zu", N_("Zulu")}
};
}
void Page::translation_request(const std::string& operation, const std::string& language, int sequence, Reply reply) {
  if (!adapter_enabled("translation")) { reply(nullptr,"Translation is disabled in Debugging settings"); return; }
  if (is_closed() || !state_->protocol_ready || target.empty() || !state_->process) {
    reply(nullptr, _("The page is not ready for translation")); return;
  }
  const auto generation = state_->navigation_generation;
  state_->process->translation_request(target, operation, language, sequence,
    [weak = weak_from_this(), generation, reply = std::move(reply)](JsonObject* result, const std::string& error) {
      auto page = weak.lock();
      if (!page || page->is_closed() || generation != page->state_->navigation_generation) {
        reply(nullptr, _("The page changed during translation")); return;
      }
      reply(result, error);
    });
}
void Page::dismiss_translation() {
  if (!state_->translation) return;
  auto ui = std::move(state_->translation);
  if (ui->poll) g_source_remove(ui->poll);
  gtk_widget_set_sensitive(ui->box, false);
  g_object_unref(ui->box);
}
void Page::update_translation(JsonObject* message, const std::string& error) {
  if (!state_->translation) return;
  auto& ui = *state_->translation;
  ui.waiting = false;
  if (!error.empty() || !message) {
    gtk_label_set_text(ui.status, error.empty() ? _("Translation is unavailable") : error.c_str());
    gtk_widget_set_sensitive(ui.translate, ui.sequence >= 0); gtk_widget_set_sensitive(ui.original, false);
    gtk_widget_set_sensitive(GTK_WIDGET(ui.language), true);
    gtk_spinner_stop(ui.spinner); gtk_widget_set_visible(GTK_WIDGET(ui.spinner), false); return;
  }
  const int sequence = json_object_get_int_member_with_default(message, "sequence", -1);
  if (sequence < 0 || (ui.sequence >= 0 && sequence != ui.sequence)) {
    dismiss_translation(); return;
  }
  ui.sequence = sequence;
  const int step = json_object_get_int_member_with_default(message, "step", 0);
  const bool translated = json_object_get_boolean_member_with_default(message, "translated", false);
  bool busy = step == 1;
  if (busy && ui.started && g_get_monotonic_time() - ui.started > 60 * G_TIME_SPAN_SECOND) {
    gtk_label_set_text(ui.status, _("Translation is taking longer than expected. Close this dialog and try again later."));
  } else if (busy) {
    if (!ui.started) ui.started = g_get_monotonic_time();
    gtk_label_set_text(ui.status, _("Translating…"));
  } else if (step == 4) {
    ui.started = 0; gtk_label_set_text(ui.status, _("Could not translate this page. Check your connection and try again."));
  } else {
    ui.started = 0; gtk_label_set_text(ui.status, translated ? _("Page translated") : _("Ready to translate"));
  }
  gtk_widget_set_sensitive(ui.translate, !busy);
  gtk_widget_set_sensitive(GTK_WIDGET(ui.language), !busy);
  gtk_widget_set_sensitive(ui.original, translated && !busy);
  gtk_widget_set_visible(GTK_WIDGET(ui.spinner), busy);
  if (busy) gtk_spinner_start(ui.spinner); else gtk_spinner_stop(ui.spinner);
  if (busy && !ui.poll) {
    auto* weak = new std::weak_ptr<Page>(weak_from_this());
    ui.poll = g_timeout_add_full(G_PRIORITY_DEFAULT, 300, +[](gpointer data) -> gboolean {
      if (auto page = static_cast<std::weak_ptr<Page>*>(data)->lock(); page && page->state_->translation) {
        page->state_->translation->poll = 0; page->poll_translation();
      }
      return G_SOURCE_REMOVE;
    }, weak, +[](gpointer data) { delete static_cast<std::weak_ptr<Page>*>(data); });
  }
}
void Page::poll_translation() {
  if (!state_->translation || state_->translation->waiting || !gtk_widget_get_mapped(state_->translation->box)) return;
  state_->translation->waiting = true;
  const auto identity = state_->translation->identity;
  translation_request("status", "", -1, [weak = weak_from_this(), identity](JsonObject* message, const std::string& error) {
    if (auto page = weak.lock(); page && page->state_->translation && page->state_->translation->identity == identity) page->update_translation(message, error);
  });
}
int Page::native_tab_id() const { return is_closed() ? -1 : state_->native_tab; }
GtkWidget* Page::create_translation_controls() {
  dismiss_translation();
  if (is_closed()) return nullptr;
  state_->translation = std::make_unique<TranslationUI>();
  auto& ui = *state_->translation;
  ui.generation = state_->navigation_generation;
  ui.box = GTK_WIDGET(g_object_ref_sink(gtk_box_new(GTK_ORIENTATION_VERTICAL, 12)));
  auto* box = ui.box;
  for (auto set_margin : {gtk_widget_set_margin_start, gtk_widget_set_margin_end, gtk_widget_set_margin_top, gtk_widget_set_margin_bottom}) set_margin(box, 12);
  auto* model = gtk_string_list_new(nullptr);
  guint selected = 18;
  std::string locale = g_get_language_names()[0];
  auto end = locale.find_first_of("_.@-"); if (end != std::string::npos) locale.resize(end);
  for (guint i = 0; i < G_N_ELEMENTS(translation_languages); ++i) {
    gtk_string_list_append(model, _(translation_languages[i].name));
    if (locale == translation_languages[i].code) selected = i;
  }
  auto* language_box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 12);
  auto* language_label = gtk_label_new(_("Translate to"));
  gtk_label_set_xalign(GTK_LABEL(language_label), 0);
  gtk_widget_set_margin_start(language_label, 12);
  gtk_widget_set_hexpand(language_label, true);
  gtk_box_append(GTK_BOX(language_box), language_label);
  ui.language = GTK_DROP_DOWN(gtk_drop_down_new(G_LIST_MODEL(model), nullptr));
  gtk_drop_down_set_enable_search(ui.language, true);
  gtk_drop_down_set_selected(ui.language, selected);
  gtk_widget_add_css_class(GTK_WIDGET(ui.language), "flat");
  // GtkDropDown's template left-aligns its popup. Center it on this button,
  // while GTK still clamps it to the monitor when close to a screen edge.
  for (auto* child = gtk_widget_get_first_child(GTK_WIDGET(ui.language)); child; child = gtk_widget_get_next_sibling(child))
    if (GTK_IS_POPOVER(child)) gtk_widget_set_halign(child, GTK_ALIGN_CENTER);
  gtk_accessible_update_property(GTK_ACCESSIBLE(ui.language), GTK_ACCESSIBLE_PROPERTY_LABEL, _("Translation language"), -1);
  gtk_box_append(GTK_BOX(language_box), GTK_WIDGET(ui.language));
  gtk_box_append(GTK_BOX(box), language_box);
  auto* status_box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
  gtk_widget_set_halign(status_box, GTK_ALIGN_CENTER);
  ui.spinner = GTK_SPINNER(gtk_spinner_new());
  gtk_widget_set_visible(GTK_WIDGET(ui.spinner), false);
  gtk_box_append(GTK_BOX(status_box), GTK_WIDGET(ui.spinner));
  ui.status = GTK_LABEL(gtk_label_new(_("Checking translation availability…")));
  gtk_label_set_wrap(ui.status, true); gtk_label_set_max_width_chars(ui.status, 32);
  gtk_box_append(GTK_BOX(status_box), GTK_WIDGET(ui.status)); gtk_box_append(GTK_BOX(box), status_box);
  auto* actions = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 12);
  gtk_widget_set_halign(actions, GTK_ALIGN_CENTER);
  ui.original = gtk_button_new_with_mnemonic(_("Show _Original"));
  ui.translate = gtk_button_new_with_mnemonic(_("_Translate"));
  gtk_widget_add_css_class(ui.translate, "suggested-action");
  gtk_widget_set_sensitive(ui.original, false); gtk_widget_set_sensitive(ui.translate, false);
  gtk_box_append(GTK_BOX(actions), ui.original); gtk_box_append(GTK_BOX(actions), ui.translate);
  gtk_box_append(GTK_BOX(box), actions);
  for (auto* button : {ui.translate, ui.original}) {
    auto* weak = new std::weak_ptr<Page>(weak_from_this());
    g_signal_connect_data(button, "clicked", G_CALLBACK(+[](GtkButton* button, gpointer data) {
      auto page = static_cast<std::weak_ptr<Page>*>(data)->lock();
      if (!page || !page->state_->translation) return;
      auto& ui = *page->state_->translation;
      if (ui.waiting || ui.sequence < 0) return;
      const auto selected = gtk_drop_down_get_selected(ui.language);
      if (selected >= G_N_ELEMENTS(translation_languages)) return;
      const bool revert = GTK_WIDGET(button) == ui.original;
      ui.waiting = true;
      gtk_widget_set_sensitive(ui.translate, false); gtk_widget_set_sensitive(ui.original, false);
      ui.started = g_get_monotonic_time(); gtk_widget_set_visible(GTK_WIDGET(ui.spinner), true); gtk_spinner_start(ui.spinner);
      gtk_label_set_text(ui.status, revert ? _("Restoring original…") : _("Translating…"));
      const auto identity = page->state_->translation->identity;
      page->translation_request(revert ? "revert" : "start", translation_languages[selected].code, ui.sequence,
        [weak = page->weak_from_this(), identity](JsonObject* message, const std::string& error) {
          if (auto page = weak.lock(); page && page->state_->translation && page->state_->translation->identity == identity) page->update_translation(message, error);
        });
    }), weak, +[](gpointer data, GClosure*) { delete static_cast<std::weak_ptr<Page>*>(data); }, G_CONNECT_DEFAULT);
  }
  auto* weak = new std::weak_ptr<Page>(weak_from_this());
  g_signal_connect_data(box, "map", G_CALLBACK(+[](GtkWidget*, gpointer data) {
    if (auto page = static_cast<std::weak_ptr<Page>*>(data)->lock()) page->poll_translation();
  }), weak, +[](gpointer data, GClosure*) { delete static_cast<std::weak_ptr<Page>*>(data); }, G_CONNECT_DEFAULT);
  return box;
}


#include "authenticator-ui.inc"
#include "vim-commands.inc"

void Page::keepassxc_request(const std::string& operation, const std::string& argument, Reply reply) {
  if (is_closed() || state_->ephemeral || !state_->process || state_->native_tab < 1) {
    reply(nullptr, _("KeePassXC is unavailable for this tab")); return;
  }
  const auto generation = state_->navigation_generation;
  state_->process->keepassxc_call(state_->native_tab, operation, argument,
    [weak = weak_from_this(), generation, reply = std::move(reply)](JsonObject* result, const std::string& error) {
      auto page = weak.lock();
      if (!page || page->is_closed() || page->state_->navigation_generation != generation) {
        reply(nullptr, _("The page changed. Try again on the current page.")); return;
      }
      reply(result, error);
    });
}
void Page::dismiss_password_manager() {
  if (!state_->passwords) return;
  auto ui = std::move(state_->passwords);
  if (ui->refresh) g_source_remove(ui->refresh);
  if (ui->closed) g_signal_handler_disconnect(ui->dialog, ui->closed);
  adw_dialog_force_close(ui->dialog); g_object_unref(ui->dialog);
}
void Page::poll_password_manager() {
  if (!state_->passwords || state_->passwords->busy) return;
  password_manager_operation("status");
}
void Page::update_password_manager(JsonObject* message, const std::string& error) {
  if (!state_->passwords) return;
  auto& ui = *state_->passwords;
  ui.busy = false; gtk_spinner_stop(ui.spinner); gtk_widget_set_visible(GTK_WIDGET(ui.spinner), false);
  if (state_->process) {
    g_autofree char* manager = gnosis_brave_password_manager(state_->process->profile.c_str());
    ui.updating = true;
    adw_combo_row_set_selected(ui.manager, !strcmp(manager, "keyslot") ? 1 : !strcmp(manager, "keepassxc") ? 2 : 0);
    ui.updating = false;
  }
  if (!error.empty() || !message) {
    adw_action_row_set_subtitle(ui.status, error.empty() ? _("KeePassXC did not respond") : error.c_str());
    gtk_widget_set_visible(ui.reconnect, true);
    for (auto* row : {ui.connect, ui.unlock, ui.lock, ui.list, ui.totp, ui.redetect}) gtk_widget_set_sensitive(row, false);
    return;
  }
  if (json_object_has_member(message, "entries")) {
    for (auto* row : ui.rows) adw_preferences_group_remove(ui.entries, row);
    ui.rows.clear();
    auto* entries = json_object_get_array_member(message, "entries");
    for (guint i = 0; entries && i < json_array_get_length(entries) && i < 200; ++i) {
      auto* entry = json_array_get_object_element(entries, i);
      const std::string id = text(entry, "id"); if (id.empty() || id.size() > 128) continue;
      auto* row = adw_action_row_new();
      adw_preferences_row_set_title(ADW_PREFERENCES_ROW(row), text(entry, "label"));
      adw_preferences_row_set_use_markup(ADW_PREFERENCES_ROW(row), false);
      adw_action_row_set_subtitle(ADW_ACTION_ROW(row), _("Fill this login"));
      gtk_list_box_row_set_activatable(GTK_LIST_BOX_ROW(row), true);
      adw_action_row_add_suffix(ADW_ACTION_ROW(row), gtk_image_new_from_icon_name("go-next-symbolic"));
      struct Fill { std::weak_ptr<Page> page; std::string id; };
      auto* fill = new Fill{weak_from_this(), id};
      g_signal_connect_data(row, "activated", G_CALLBACK(+[](AdwActionRow*, gpointer data) {
        auto* fill = static_cast<Fill*>(data);
        if (auto page = fill->page.lock()) page->password_manager_operation("fill", quote(fill->id));
      }), fill, +[](gpointer data, GClosure*) { delete static_cast<Fill*>(data); }, G_CONNECT_DEFAULT);
      adw_preferences_group_add(ui.entries, row); ui.rows.push_back(row);
    }
    adw_preferences_group_set_description(ui.entries, ui.rows.empty() ? _("No matching logins were found for this website.") : nullptr);
    gtk_widget_set_visible(GTK_WIDGET(ui.entries), true);
    gtk_widget_set_sensitive(ui.list, true); return;
  }
  const bool available = json_object_get_boolean_member_with_default(message, "available", false);
  const bool locked = json_object_get_boolean_member_with_default(message, "locked", true);
  const bool associated = json_object_get_boolean_member_with_default(message, "associated", false);
  const bool website = json_object_get_boolean_member_with_default(message, "website", false);
  adw_action_row_set_subtitle(ui.status, !available ? _("Open your password manager and enable Browser Integration") :
    locked ? _("Database locked — unlock it in your password manager") : !associated ? _("Authorize Gnosis to access this database") : _("Connected to your database"));
  gtk_widget_set_visible(ui.reconnect, !available);
  gtk_widget_set_visible(ui.unlock, available && locked);
  gtk_widget_set_visible(ui.connect, available && !associated);
  gtk_widget_set_visible(ui.lock, available && !locked);
  gtk_widget_set_sensitive(ui.unlock, available);
  gtk_widget_set_sensitive(ui.connect, available);
  gtk_widget_set_sensitive(ui.lock, available && !locked);
  gtk_widget_set_sensitive(ui.list, available && !locked && associated && website);
  gtk_widget_set_sensitive(ui.totp, available && !locked && associated && website);
  gtk_widget_set_sensitive(ui.redetect, website);
  if (json_object_get_boolean_member_with_default(message, "redetected", false))
    adw_action_row_set_subtitle(ui.status, _("Login fields rescanned"));
  ui.updating = true;
  adw_switch_row_set_active(ui.passkeys, json_object_get_boolean_member_with_default(message, "passkeys", true));
  adw_switch_row_set_active(ui.auto_unlock, json_object_get_boolean_member_with_default(message, "autoUnlock", true));
  ui.updating = false;
  if (locked || !associated) {
    for (auto* row : ui.rows) adw_preferences_group_remove(ui.entries, row);
    ui.rows.clear(); gtk_widget_set_visible(GTK_WIDGET(ui.entries), false);
  }
}
void Page::password_manager_operation(std::string operation, std::string argument) {
  if (!state_->passwords || state_->passwords->busy) return;
  if (operation == "manager") {
    g_autoptr(GError) error = nullptr;
    if (!state_->process || !gnosis_brave_password_manager_set(state_->process->profile.c_str(), argument.c_str(), &error)) {
      update_password_manager(nullptr, error ? error->message : _("The browser connection is unavailable")); return;
    }
    operation = "reconnect"; argument = "null";
  }
  const bool fill = operation == "fill" || operation == "totp";
  const auto identity = state_->passwords->identity;
  if (fill) { dismiss_password_manager(); focus(); }
  else {
    auto& ui = *state_->passwords; ui.busy = true;
    gtk_widget_set_visible(GTK_WIDGET(ui.spinner), true); gtk_spinner_start(ui.spinner);
    if (operation != "status") adw_action_row_set_subtitle(ui.status,
      operation == "unlock" || operation == "associate" ? _("Continue in your password manager…") : _("Working…"));
  }
  keepassxc_request(operation, argument,
    [weak = weak_from_this(), identity, fill](JsonObject* result, const std::string& error) {
      auto page = weak.lock(); if (!page || page->is_closed()) return;
      if (fill) {
        if (!error.empty()) {
          auto* root = gtk_widget_get_root(page->widget()); if (!root || !GTK_IS_WINDOW(root)) return;
          auto* dialog = adw_alert_dialog_new(_("Could Not Fill Login"), error.c_str());
          adw_alert_dialog_add_response(ADW_ALERT_DIALOG(dialog), "close", _("_Close"));
          adw_alert_dialog_set_close_response(ADW_ALERT_DIALOG(dialog), "close");
          adw_dialog_present(dialog, GTK_WIDGET(root));
        }
        return;
      }
      if (page->state_->passwords && page->state_->passwords->identity == identity)
        page->update_password_manager(result, error);
    });
}
void Page::show_password_manager() {
  if (!adapter_enabled("keepassxc")) { g_warning("KeePassXC is disabled in Debugging settings"); return; }
  if (is_closed()) return;
  auto* root = gtk_widget_get_root(state_->content); if (!root || !GTK_IS_WINDOW(root)) return;
  if (state_->passwords) { adw_dialog_present(state_->passwords->dialog, GTK_WIDGET(root)); return; }
  if (state_->ephemeral) {
    auto* dialog = adw_alert_dialog_new(_("KeePassXC"), _("KeePassXC is not enabled in private tabs."));
    adw_alert_dialog_add_response(ADW_ALERT_DIALOG(dialog), "close", _("_Close"));
    adw_alert_dialog_set_close_response(ADW_ALERT_DIALOG(dialog), "close");
    adw_dialog_present(dialog, GTK_WIDGET(root)); return;
  }
  state_->passwords = std::make_unique<PasswordUI>(); auto& ui = *state_->passwords;
  ui.dialog = ADW_DIALOG(g_object_ref_sink(adw_preferences_dialog_new()));
  adw_dialog_set_title(ui.dialog, _("Password Manager"));
  adw_dialog_set_content_width(ui.dialog, 480); adw_dialog_set_content_height(ui.dialog, 540);
  auto* page = ADW_PREFERENCES_PAGE(adw_preferences_page_new());
  adw_preferences_dialog_add(ADW_PREFERENCES_DIALOG(ui.dialog), page);
  auto* connection = ADW_PREFERENCES_GROUP(adw_preferences_group_new());
  adw_preferences_group_set_description(connection, _("Your database password is entered only in your password manager."));
  adw_preferences_page_add(page, connection);
  ui.manager = ADW_COMBO_ROW(adw_combo_row_new());
  adw_preferences_row_set_title(ADW_PREFERENCES_ROW(ui.manager), _("Password Manager"));
  const char* managers[] = {_("Automatic"), "Keyslot", "KeePassXC", nullptr};
  g_autoptr(GtkStringList) manager_names = gtk_string_list_new(managers);
  adw_combo_row_set_model(ui.manager, G_LIST_MODEL(manager_names));
  g_autofree char* selected_manager = gnosis_brave_password_manager(state_->process->profile.c_str());
  adw_combo_row_set_selected(ui.manager, !strcmp(selected_manager, "keyslot") ? 1 : !strcmp(selected_manager, "keepassxc") ? 2 : 0);
  auto* manager_page = new std::weak_ptr<Page>(weak_from_this());
  g_signal_connect_data(ui.manager, "notify::selected", G_CALLBACK(+[](AdwComboRow* row, GParamSpec*, gpointer data) {
    auto page = static_cast<std::weak_ptr<Page>*>(data)->lock();
    if (!page || !page->state_->passwords || page->state_->passwords->updating) return;
    const auto selected = adw_combo_row_get_selected(row);
    if (selected < 3) page->password_manager_operation("manager", selected == 1 ? "keyslot" : selected == 2 ? "keepassxc" : "auto");
  }), manager_page, +[](gpointer data, GClosure*) { delete static_cast<std::weak_ptr<Page>*>(data); }, G_CONNECT_DEFAULT);
  adw_preferences_group_add(connection, GTK_WIDGET(ui.manager));
  ui.status = ADW_ACTION_ROW(adw_action_row_new());
  adw_action_row_set_subtitle_selectable(ui.status, true);
  adw_preferences_row_set_use_markup(ADW_PREFERENCES_ROW(ui.status), false);
  adw_preferences_row_set_title(ADW_PREFERENCES_ROW(ui.status), _("Database Connection"));
  adw_action_row_set_subtitle(ui.status, _("Connecting to your password manager…"));
  ui.spinner = GTK_SPINNER(gtk_spinner_new()); adw_action_row_add_suffix(ui.status, GTK_WIDGET(ui.spinner));
  adw_preferences_group_add(connection, GTK_WIDGET(ui.status));
  auto* actions = ADW_PREFERENCES_GROUP(adw_preferences_group_new());
  adw_preferences_group_set_title(actions, _("This Website")); adw_preferences_page_add(page, actions);
  auto row = [this](AdwPreferencesGroup* group, const char* title, const char* icon, const char* operation) {
    auto* widget = adw_button_row_new();
    adw_preferences_row_set_title(ADW_PREFERENCES_ROW(widget), title);
    adw_button_row_set_start_icon_name(ADW_BUTTON_ROW(widget), icon);
    struct Action { std::weak_ptr<Page> page; std::string operation; };
    auto* action = new Action{weak_from_this(), operation};
    g_signal_connect_data(widget, "activated", G_CALLBACK(+[](AdwButtonRow*, gpointer data) {
      auto* action = static_cast<Action*>(data);
      if (auto page = action->page.lock()) page->password_manager_operation(action->operation);
    }), action, +[](gpointer data, GClosure*) { delete static_cast<Action*>(data); }, G_CONNECT_DEFAULT);
    adw_preferences_group_add(group, widget); return widget;
  };
  ui.connect = row(connection, _("Connect Database…"), "network-connect-symbolic", "associate");
  ui.unlock = row(connection, _("Unlock Database…"), "changes-allow-symbolic", "unlock");
  ui.lock = row(connection, _("Lock Database"), "changes-prevent-symbolic", "lock");
  ui.reconnect = row(connection, _("Reconnect"), "view-refresh-symbolic", "reconnect");
  auto* forget = adw_button_row_new();
  adw_preferences_row_set_title(ADW_PREFERENCES_ROW(forget), _("Forget Browser Connections…"));
  adw_button_row_set_start_icon_name(ADW_BUTTON_ROW(forget), "edit-delete-symbolic");
  auto* forget_page = new std::weak_ptr<Page>(weak_from_this());
  g_signal_connect_data(forget, "activated", G_CALLBACK(+[](AdwButtonRow*, gpointer data) {
    auto page = static_cast<std::weak_ptr<Page>*>(data)->lock();
    if (!page || !page->state_->passwords || page->state_->passwords->busy) return;
    auto* alert = ADW_ALERT_DIALOG(adw_alert_dialog_new(_("Forget Browser Connections?"),
      _("Remove saved database connections from this Gnosis profile. Your vault entries will be kept. You can connect again in your password manager.")));
    adw_alert_dialog_add_responses(alert, "cancel", _("Cancel"), "forget", _("Forget Connections"), nullptr);
    adw_alert_dialog_set_response_appearance(alert, "forget", ADW_RESPONSE_DESTRUCTIVE);
    adw_alert_dialog_set_default_response(alert, "cancel");
    adw_alert_dialog_set_close_response(alert, "cancel");
    auto* weak = new std::weak_ptr<Page>(page);
    g_signal_connect_data(alert, "response", G_CALLBACK(+[](AdwAlertDialog*, const char* response, gpointer data) {
      if (!strcmp(response, "forget")) if (auto page = static_cast<std::weak_ptr<Page>*>(data)->lock())
        page->password_manager_operation("forget");
    }), weak, +[](gpointer data, GClosure*) { delete static_cast<std::weak_ptr<Page>*>(data); }, G_CONNECT_DEFAULT);
    adw_dialog_present(ADW_DIALOG(alert), GTK_WIDGET(page->state_->passwords->dialog));
  }), forget_page, +[](gpointer data, GClosure*) { delete static_cast<std::weak_ptr<Page>*>(data); }, G_CONNECT_DEFAULT);
  adw_preferences_group_add(connection, forget);
  ui.list = row(actions, _("Choose a Login…"), "dialog-password-symbolic", "entries");
  ui.totp = row(actions, _("Fill One-Time Code"), "document-edit-symbolic", "totp");
  ui.redetect = row(actions, _("Redetect Login Fields"), "view-refresh-symbolic", "redetect");
  for (auto* widget : {ui.connect, ui.unlock, ui.lock, ui.list, ui.totp, ui.redetect}) gtk_widget_set_sensitive(widget, false);
  ui.entries = ADW_PREFERENCES_GROUP(adw_preferences_group_new());
  adw_preferences_group_set_title(ui.entries, _("Matching Logins"));
  gtk_widget_set_visible(GTK_WIDGET(ui.entries), false); adw_preferences_page_add(page, ui.entries);
  auto* settings = ADW_PREFERENCES_GROUP(adw_preferences_group_new()); adw_preferences_page_add(page, settings);
  adw_preferences_group_set_title(settings, _("Browser Integration"));
  ui.passkeys = ADW_SWITCH_ROW(adw_switch_row_new());
  adw_preferences_row_set_title(ADW_PREFERENCES_ROW(ui.passkeys), _("Use Password Manager for Passkeys"));
  adw_action_row_set_subtitle(ADW_ACTION_ROW(ui.passkeys), _("Store and use passkeys in your database"));
  ui.auto_unlock = ADW_SWITCH_ROW(adw_switch_row_new());
  adw_preferences_row_set_title(ADW_PREFERENCES_ROW(ui.auto_unlock), _("Ask to Unlock When Filling"));
  adw_action_row_set_subtitle(ADW_ACTION_ROW(ui.auto_unlock), _("Show your password manager when you interact with a login field"));
  for (auto* setting : {ui.passkeys, ui.auto_unlock}) {
    adw_preferences_group_add(settings, GTK_WIDGET(setting));
    auto* weak = new std::weak_ptr<Page>(weak_from_this());
    g_signal_connect_data(setting, "notify::active", G_CALLBACK(+[](AdwSwitchRow* row, GParamSpec*, gpointer data) {
      auto page = static_cast<std::weak_ptr<Page>*>(data)->lock();
      if (!page || !page->state_->passwords || page->state_->passwords->updating) return;
      auto& ui = *page->state_->passwords;
      page->password_manager_operation(row == ui.passkeys ? "passkeys" : "auto-unlock", adw_switch_row_get_active(row) ? "true" : "false");
    }), weak, +[](gpointer data, GClosure*) { delete static_cast<std::weak_ptr<Page>*>(data); }, G_CONNECT_DEFAULT);
  }
  auto* weak = new std::weak_ptr<Page>(weak_from_this());
  ui.closed = g_signal_connect_data(ui.dialog, "closed", G_CALLBACK(+[](AdwDialog*, gpointer data) {
    if (auto page = static_cast<std::weak_ptr<Page>*>(data)->lock()) page->dismiss_password_manager();
  }), weak, +[](gpointer data, GClosure*) { delete static_cast<std::weak_ptr<Page>*>(data); }, G_CONNECT_DEFAULT);
  auto* poll = new std::weak_ptr<Page>(weak_from_this());
  ui.refresh = g_timeout_add_full(G_PRIORITY_DEFAULT, 2000, +[](gpointer data) -> gboolean {
    auto page = static_cast<std::weak_ptr<Page>*>(data)->lock();
    if (!page || !page->state_->passwords) return G_SOURCE_REMOVE;
    page->poll_password_manager(); return G_SOURCE_CONTINUE;
  }, poll, +[](gpointer data) { delete static_cast<std::weak_ptr<Page>*>(data); });
  adw_dialog_present(ui.dialog, GTK_WIDGET(root)); poll_password_manager();
}


void Page::update_vim(int context) {
  if (!adapter_enabled("vim")) return;
  auto unique = state_->metadata_unique.find(context);
  auto source = state_->menu_sessions.find(context);
  if (is_closed() || unique == state_->metadata_unique.end() || source == state_->menu_sessions.end()) return;
  g_autoptr(GdkRGBA) accent = adw_style_manager_get_accent_color_rgba(adw_style_manager_get_default());
  g_autofree char* color = gdk_rgba_to_string(accent);
  const std::string expression = std::string("globalThis.gnosisVim?.setEnabled(") +
    (vim_mode && !state_->extension_popup ? "true" : "false") + "," + quote(color) + ")";
  state_->process->command("Runtime.evaluate", "{\"uniqueContextId\":" + quote(unique->second) +
    ",\"expression\":" + quote(expression) + "}", source->second);
}
void Page::update_preferences() {
  update_web_theme();
  for (const auto& [context, frame] : state_->metadata_contexts) update_vim(context);
}
void Page::update_user_content() {
  if (session.empty() || is_closed()) return;
  auto& state = *state_;
  update_web_theme();
  if (state.applied_user_script == user_javascript) return;
  state.applied_user_script = user_javascript;
  const auto generation = ++state.user_content_generation;
  if (!state.user_script_id.empty()) {
    command("Page.removeScriptToEvaluateOnNewDocument", "{\"identifier\":" + quote(state.user_script_id) + "}");
    state.user_script_id.clear();
  }
  if (!adapter_enabled("user-content") || user_javascript.empty()) return;
  const std::string body = "(()=>{if(!['http:','https:','file:'].includes(location.protocol))return;const run=()=>{\n" +
    user_javascript + "\n};if(document.readyState==='loading')addEventListener('DOMContentLoaded',run,{once:true});else run();})()";
  command("Page.addScriptToEvaluateOnNewDocument", "{\"source\":" + quote(body) + "}",
    [weak = weak_from_this(), generation](JsonObject* result, const std::string&) {
      if (auto page = weak.lock(); page && !page->is_closed()) {
        const std::string id = text(result, "identifier");
        if (generation != page->state_->user_content_generation) {
          if (!id.empty()) page->command("Page.removeScriptToEvaluateOnNewDocument", "{\"identifier\":" + quote(id) + "}");
        } else page->state_->user_script_id = id;
      }
    });
  command("Runtime.evaluate", "{\"expression\":" + quote(body) + "}");
}
void Page::print_pdf(const std::string& path, GtkPageSetup* setup, GCancellable* cancellable,
                     std::function<void(const std::string&)> done) {
  if (is_closed() || !state_->protocol_ready) { done("The tab is not ready for printing"); return; }
  auto* paper = gtk_page_setup_get_paper_size(setup);
  const auto orientation = gtk_page_setup_get_orientation(setup);
  const bool landscape = orientation == GTK_PAGE_ORIENTATION_LANDSCAPE || orientation == GTK_PAGE_ORIENTATION_REVERSE_LANDSCAPE;
  g_autoptr(JsonObject) options = json_object_new();
  json_object_set_boolean_member(options, "landscape", landscape);
  json_object_set_boolean_member(options, "printBackground", true);
  json_object_set_double_member(options, "paperWidth", gtk_paper_size_get_width(paper, GTK_UNIT_INCH));
  json_object_set_double_member(options, "paperHeight", gtk_paper_size_get_height(paper, GTK_UNIT_INCH));
  json_object_set_double_member(options, "marginTop", gtk_page_setup_get_top_margin(setup, GTK_UNIT_INCH));
  json_object_set_double_member(options, "marginBottom", gtk_page_setup_get_bottom_margin(setup, GTK_UNIT_INCH));
  json_object_set_double_member(options, "marginLeft", gtk_page_setup_get_left_margin(setup, GTK_UNIT_INCH));
  json_object_set_double_member(options, "marginRight", gtk_page_setup_get_right_margin(setup, GTK_UNIT_INCH));
  g_autoptr(JsonNode) node = json_node_new(JSON_NODE_OBJECT);
  json_node_set_object(node, options);
  g_autofree char* params = json_to_string(node, false);
  auto cancel = std::shared_ptr<GCancellable>(G_CANCELLABLE(g_object_ref(cancellable)), g_object_unref);
  const auto generation = state_->navigation_generation;
  command("Page.printToPDF", params, [weak = weak_from_this(), generation, path, cancel, done = std::move(done)](JsonObject* result, const std::string& error) {
    auto page = weak.lock();
    if (g_cancellable_is_cancelled(cancel.get())) { done("Printing was cancelled"); return; }
    if (!page || page->is_closed() || page->state_->navigation_generation != generation) {
      done("The document changed or its tab was closed while printing"); return;
    }
    if (!result) { done(error.empty() ? "Brave could not render this document for printing" : error); return; }
    const char* encoded = text(result, "data");
    gsize size = 0;
    g_autofree guchar* pdf = g_base64_decode(encoded, &size);
    if (size < 5 || memcmp(pdf, "%PDF-", 5)) { done("Brave returned an invalid print document"); return; }
    g_autoptr(GError) failure = nullptr;
    if (!g_file_set_contents_full(path.c_str(), reinterpret_cast<const char*>(pdf), size,
                                 G_FILE_SET_CONTENTS_CONSISTENT, 0600, &failure)) {
      done(failure->message); return;
    }
    done("");
  });
}
void Page::set_zoom(double factor) {
  if (is_closed() || !std::isfinite(factor)) return;
  state_->zoom_requested = std::clamp(factor,.1,10.);
  ++state_->zoom_revision; state_->zoom_pending = true; apply_zoom();
}
void Page::receive_zoom(double factor) {
  auto& s = *state_;
  if (s.disposed || loading || s.zoom_busy || s.zoom_pending || !std::isfinite(factor) || factor < .1 || factor > 10) return;
  s.zoom_requested = s.zoom_applied = factor;
  if (zoom_changed) zoom_changed(factor);
}
void Page::apply_zoom() {
  auto& s = *state_;
  if (s.disposed || loading || s.zoom_busy || !s.zoom_pending || !s.attached || s.window < 0 || !s.process->integration_ready) return;
  s.zoom_pending = false; s.zoom_busy = true;
  auto weak = weak_from_this();
  const guint64 revision = s.zoom_revision;
  const std::string expected = uri;
  // Browser and extension window IDs share Chromium's SessionID. App windows
  // have exactly one tab; never fall back to whichever tab happens to be active.
  const std::string script = "(async()=>{const tabs=await chrome.tabs.query({windowId:" + std::to_string(s.window) +
    "});if(tabs.length!==1)throw Error('The native browser window has no unique tab');const id=tabs[0].id;"
    "await chrome.tabs.setZoomSettings(id,{mode:'automatic',scope:'per-tab'});await chrome.tabs.setZoom(id," +
    std::to_string(s.zoom_requested) + ");return {tabId:id,factor:await chrome.tabs.getZoom(id)};})()";
  s.process->command("Runtime.evaluate","{\"expression\":" + quote(script) + ",\"awaitPromise\":true,\"returnByValue\":true}",
    s.process->integration_session,[weak,revision,expected](JsonObject* result,const std::string& error) {
      auto self = weak.lock(); if (!self || self->is_closed()) return;
      auto& s = *self->state_; s.zoom_busy = false;
      auto* value = object(object(result,"result"),"value");
      const bool current = expected == self->uri && revision == s.zoom_revision;
      if (!value || object(result,"exceptionDetails")) {
        if (current) {
          g_warning("Native Brave zoom failed: %s",error.empty() ? text(object(object(result,"exceptionDetails"),"exception"),"description") : error.c_str());
          s.zoom_requested = s.zoom_applied;
          if (self->zoom_changed) self->zoom_changed(s.zoom_applied);
        }
      } else {
        const int tab = json_object_get_int_member_with_default(value,"tabId",-1);
        const double factor = json_object_get_double_member_with_default(value,"factor",0);
        if (tab >= 0 && std::isfinite(factor) && factor >= .1 && factor <= 10) {
          if (s.native_tab >= 0) s.process->zoom_tabs.erase(s.native_tab);
          s.native_tab = tab; s.process->zoom_tabs[tab] = self; s.zoom_applied = factor;
          if (current && self->zoom_changed) self->zoom_changed(factor);
        }
      }
      self->apply_zoom();
    });
}
void Page::show_context_menu(JsonObject* hit, int context) {
  if (!adapter_enabled("context-menu")) return;
  auto& s = *state_;
  if (s.disposed || !s.surface || !gtk_widget_get_mapped(s.surface)) return;
  dismiss_context_menu();
  auto* popover = gtk_popover_menu_new_from_model(nullptr);
  s.context_menu = popover;
  gtk_widget_set_parent(popover, s.surface);
  gtk_popover_set_has_arrow(GTK_POPOVER(popover), FALSE);
  auto* actions = g_simple_action_group_new();
  auto* menu = g_menu_new();
  auto weak = weak_from_this();
  const std::string expected = uri;
  const std::string document = s.menu_contexts.at(context);
  const std::string menu_session = s.menu_sessions.at(context);
  const auto generation = s.navigation_generation;
  auto current = [weak, expected, context, document, generation]() -> std::shared_ptr<Page> {
    auto page = weak.lock();
    if (!page || page->is_closed() || page->uri != expected || page->state_->navigation_generation != generation || !page->state_->mapped) return {};
    auto found = page->state_->menu_contexts.find(context);
    return found != page->state_->menu_contexts.end() && found->second == document ? page : nullptr;
  };
  int index = 0;
  auto append = [&](GMenu* section, const char* label, std::function<void(Page&)> callback, bool enabled = true, const char* icon = nullptr, bool before_close = false) {
    const std::string name = "item" + std::to_string(index++);
    auto* action = g_simple_action_new(name.c_str(), nullptr);
    g_simple_action_set_enabled(action, enabled);
    auto* invoke = new std::function<void()>([current, callback = std::move(callback), before_close] {
      if (auto page = current()) {
        if (before_close) {
          // Publish GTK clipboard data while the menu's real activation and
          // Wayland input serial still belong to a live popup surface.
          callback(*page);
          page->dismiss_context_menu();
          return;
        }
        page->dismiss_context_menu();
        // The popover must release its keyboard grab before native edit chords.
        auto* pending = new std::function<void()>([current, callback] {
          if (auto page = current()) callback(*page);
        });
        g_idle_add_full(G_PRIORITY_DEFAULT_IDLE, +[](gpointer data) -> gboolean {
          (*static_cast<std::function<void()>*>(data))(); return G_SOURCE_REMOVE;
        }, pending, +[](gpointer data) { delete static_cast<std::function<void()>*>(data); });
      }
    });
    g_signal_connect_data(action, "activate", G_CALLBACK(+[](GSimpleAction*, GVariant*, gpointer data) {
      // Closing the popover can destroy its action group and this signal's
      // closure. Keep the invocation alive before it unparents the menu.
      auto callback = *static_cast<std::function<void()>*>(data);
      callback();
    }), invoke, +[](gpointer data, GClosure*) { delete static_cast<std::function<void()>*>(data); }, GConnectFlags(0));
    g_action_map_add_action(G_ACTION_MAP(actions), G_ACTION(action)); g_object_unref(action);
    auto* item = g_menu_item_new(label, ("native-context." + name).c_str());
    if (icon) {
      auto* themed = g_themed_icon_new(icon);
      auto* serialized = g_icon_serialize(G_ICON(themed));
      g_menu_item_set_attribute_value(item,"verb-icon",serialized);
      g_variant_unref(serialized); g_object_unref(themed);
    }
    g_menu_append_item(section,item); g_object_unref(item);
    return action;
  };
  auto section = [&] { return g_menu_new(); };
  auto finish = [&](GMenu* value) {
    if (g_menu_model_get_n_items(G_MENU_MODEL(value))) g_menu_append_section(menu, nullptr, G_MENU_MODEL(value));
    g_object_unref(value);
  };
  auto shell_action = [](const char* name, std::string value) {
    return [name, value](Page& page) {
      gtk_widget_activate_action(page.widget(), name, "s", value.c_str());
    };
  };
  auto copy = [](std::string value) {
    return [value](Page& page) { gdk_clipboard_set_text(gtk_widget_get_clipboard(page.widget()), value.c_str()); };
  };
  auto navigable = [](const std::string& value) {
    g_autofree char* scheme = g_uri_parse_scheme(value.c_str());
    return scheme && (!g_ascii_strcasecmp(scheme,"https") || !g_ascii_strcasecmp(scheme,"http"));
  };
  const std::string link = text(hit,"link"), image = text(hit,"image"), selection = text(hit,"selection");
  auto* media_node = json_object_get_member(hit, "media");
  auto* media = media_node && JSON_NODE_HOLDS_OBJECT(media_node) ? json_node_get_object(media_node) : nullptr;
  const std::string media_kind = text(media, "kind"), media_source = text(media, "src");
  const std::string target_token = text(hit, "target");
  const bool has_media = media && (media_kind == "video" || media_kind == "audio") &&
    !target_token.empty() && target_token.size() <= 32;
  auto media_action = [document, target_token, menu_session](const char* operation) {
    return [document, target_token, menu_session, operation = std::string(operation)](Page& page) {
      // The target is a private-world WeakRef captured by the real context
      // event, never a selector or an object supplied by webpage JavaScript.
      const auto expression = "globalThis.__gnosisContextMenuAction(" + quote(target_token) + "," + quote(operation) + ")";
      page.state_->process->command("Runtime.evaluate", "{\"uniqueContextId\":" + quote(document) + ",\"expression\":" +
        quote(expression) + ",\"userGesture\":true,\"awaitPromise\":true,\"returnByValue\":true}", menu_session);
    };
  };
  const bool editable = json_object_get_boolean_member_with_default(hit,"editable",false);
  auto* links = section();
  if (!link.empty()) {
    if (navigable(link)) {
      append(links, _("Open Link in New _Tab"), shell_action("popup.open-selection-in-new-tab",link));
      append(links, _("Open Link in New _Window"), shell_action("popup.open-selection-in-new-window",link));
      if (!s.ephemeral) append(links, _("Open Link in I_ncognito Window"), shell_action("popup.open-selection-in-incognito-window",link));
    }
    append(links, _("Copy Link _Address"), copy(link), true, nullptr, true);
  }
  if (!image.empty()) {
    if (navigable(image)) append(links, _("Open Image in New Tab"), shell_action("popup.open-selection-in-new-tab",image));
    struct ImageCopy { GdkTexture* texture = nullptr; ~ImageCopy() { g_clear_object(&texture); } };
    auto pixels = std::make_shared<ImageCopy>();
    auto* copy_image = append(links, _("_Copy Image"), [pixels](Page& page) {
      if (pixels->texture) gdk_clipboard_set_texture(gtk_widget_get_clipboard(page.widget()), pixels->texture);
    }, false, nullptr, true);
    auto action = std::shared_ptr<GSimpleAction>(G_SIMPLE_ACTION(g_object_ref(copy_image)), g_object_unref);
    // Prepare the real resource while the menu is open. Publishing the ready
    // texture during activation retains GTK's Wayland clipboard input serial.
    // Use this frame's Brave cache/network context, including cross-origin
    // images and its private cookies; never fetch through a shell Soup session.
    auto decode = [current, pixels, action](const std::string& bytes) {
      if (!current() || bytes.empty() || bytes.size() > 64 * 1024 * 1024) return;
      auto* contents = g_bytes_new(bytes.data(), bytes.size());
      auto* input = g_memory_input_stream_new_from_bytes(contents); g_bytes_unref(contents);
      using Decoded = std::function<void(GdkPixbuf*)>;
      auto* done = new Decoded([current, pixels, action](GdkPixbuf* pixbuf) {
        if (!current() || !pixbuf) return;
        pixels->texture = gdk_texture_new_for_pixbuf(pixbuf);
        g_simple_action_set_enabled(action.get(), true);
      });
      gdk_pixbuf_new_from_stream_async(input, nullptr, +[](GObject*, GAsyncResult* result, gpointer data) {
        std::unique_ptr<Decoded> done(static_cast<Decoded*>(data));
        g_autoptr(GError) error = nullptr;
        auto* pixbuf = gdk_pixbuf_new_from_stream_finish(result, &error);
        (*done)(pixbuf); g_clear_object(&pixbuf);
      }, done);
      g_object_unref(input);
    };
    const auto frame = s.menu_frames.at(context);
    s.process->command("Page.getResourceContent", "{\"frameId\":" + quote(frame) + ",\"url\":" + quote(image) + "}",
      menu_session, [current, image, frame, decode, menu_session](JsonObject* result, const std::string& error) {
        auto page = current(); if (!page) return;
        if (result && error.empty()) {
          if (json_object_get_boolean_member_with_default(result, "base64Encoded", false)) {
            gsize size = 0; g_autofree guchar* bytes = g_base64_decode(text(result, "content"), &size);
            decode(std::string(reinterpret_cast<const char*>(bytes), size));
          } else decode(text(result, "content"));
          return;
        }
        page->state_->process->command("Network.loadNetworkResource", "{\"frameId\":" + quote(frame) + ",\"url\":" + quote(image) +
          ",\"options\":{\"disableCache\":false,\"includeCredentials\":true}}",
          menu_session, [current, decode, process = page->state_->process, session = menu_session](JsonObject* result, const std::string&) {
            auto* resource = object(result, "resource"); const std::string stream = text(resource, "stream");
            if (stream.empty()) return;
            auto page = current();
            if (!page || !json_object_get_boolean_member_with_default(resource, "success", false)) {
              process->command("IO.close", "{\"handle\":" + quote(stream) + "}", session);
              return;
            }
            auto read = std::make_shared<ResourceRead>(); read->process = page->state_->process;
            read->session = session; read->stream = stream; read->limit = 64 * 1024 * 1024; read->complete = decode;
            read->next();
          });
      });
    append(links, _("Copy Image _Address"), copy(image), true, nullptr, true);
  }
  finish(links);
  if (has_media) {
    auto flag = [media](const char* name) { return json_object_get_boolean_member_with_default(media, name, false); };
    auto* playback = section();
    append(playback, flag("paused") ? _("_Play") : _("_Pause"), media_action(flag("paused") ? "play" : "pause"), !flag("error"));
    append(playback, flag("muted") ? _("_Unmute") : _("_Mute"), media_action(flag("muted") ? "unmute" : "mute"));
    append(playback, flag("loop") ? _("Stop _Looping") : _("_Loop"), media_action(flag("loop") ? "unloop" : "loop"));
    append(playback, flag("controls") ? _("Hide _Controls") : _("Show _Controls"),
      media_action(flag("controls") ? "hide-controls" : "show-controls"));
    if (flag("fullscreen"))
      append(playback, flag("inFullscreen") ? _("Leave _Fullscreen") : _("_Fullscreen"), media_action("fullscreen"));
    finish(playback);
    if (!media_source.empty()) {
      auto* source = section();
      if (navigable(media_source))
        append(source, media_kind == "video" ? _("Open Video in New Tab") : _("Open Audio in New Tab"),
          shell_action("popup.open-selection-in-new-tab", media_source));
      append(source, media_kind == "video" ? _("Copy Video _Address") : _("Copy Audio _Address"),
        copy(media_source), true, nullptr, true);
      finish(source);
    }
  }
  auto* editing = section();
  if (editable) {
    append(editing, _("_Undo"), [](Page& page) { page.edit("Undo"); });
    append(editing, _("Re_do"), [](Page& page) { page.edit("Redo"); });
    append(editing, _("Cu_t"), [](Page& page) { page.edit("Cut"); }, !selection.empty());
    append(editing, _("_Copy"), copy(selection), !selection.empty(), nullptr, true);
    append(editing, _("_Paste"), [](Page& page) { page.edit("Paste"); });
    append(editing, _("Select _All"), [](Page& page) { page.edit("SelectAll"); });
  } else if (!selection.empty()) append(editing, _("_Copy"), copy(selection), true, nullptr, true);
  if (!selection.empty()) append(editing, _("Search the Web for Selection"), shell_action("popup.search-selection",selection));
  finish(editing);
  if (link.empty() && image.empty() && selection.empty() && !editable && !has_media) {
    auto* navigation = section();
    append(navigation, _("_Back"), [](Page& page) { page.back(); }, can_back,"go-previous-symbolic");
    append(navigation, _("_Forward"), [](Page& page) { page.forward(); }, can_forward,"go-next-symbolic");
    append(navigation, _("_Reload"), [](Page& page) { page.reload(); }, true,"view-refresh-symbolic");
    append(navigation, _("Add Boo_kmark…"), [](Page& page) { gtk_widget_activate_action(page.widget(), "popup.context-bookmark-page", nullptr); },true,"non-starred-symbolic");
    auto* navigation_row = g_menu_item_new_section(nullptr,G_MENU_MODEL(navigation));
    g_menu_item_set_attribute(navigation_row,"display-hint","s","horizontal-buttons");
    g_menu_append_item(menu,navigation_row); g_object_unref(navigation_row); g_object_unref(navigation);
    auto* page_actions = section();
    append(page_actions, _("Select _All"), [](Page& page) { page.edit("SelectAll"); });
    finish(page_actions);
  }
  auto* tools = section();
  append(tools, _("Open _Developer Tools"), [](Page& page) { page.show_devtools(); });
  finish(tools);
  gtk_widget_insert_action_group(popover,"native-context",G_ACTION_GROUP(actions)); g_object_unref(actions);
  gtk_popover_menu_set_menu_model(GTK_POPOVER_MENU(popover),G_MENU_MODEL(menu)); g_object_unref(menu);
  // GTK's horizontal menu section distributes equal-width buttons across the
  // row. Keep them flat rather than using the section's default linked style.
  std::function<void(GtkWidget*)> widen_navigation = [&](GtkWidget* widget) {
    if (gtk_widget_has_css_class(widget,"horizontal-buttons")) {
      gtk_widget_remove_css_class(widget,"linked");
      for (auto* button = gtk_widget_get_first_child(widget); button; button = gtk_widget_get_next_sibling(button)) {
        gtk_widget_add_css_class(button,"flat");
        gtk_widget_set_hexpand(button,TRUE);
        gtk_widget_set_size_request(button,48,-1);
      }
      return;
    }
    for (auto* child = gtk_widget_get_first_child(widget); child; child = gtk_widget_get_next_sibling(child)) widen_navigation(child);
  };
  widen_navigation(popover);
  g_signal_connect_data(popover,"closed",G_CALLBACK(+[](GtkPopover* popover, gpointer data) {
    if (auto page = static_cast<std::weak_ptr<Page>*>(data)->lock(); page && page->state_->context_menu == GTK_WIDGET(popover))
      page->state_->context_menu = nullptr;
    // GtkModelButton closes its menu before resolving/activating the action.
    // Keep the action group parented until that activation has returned.
    g_idle_add_full(G_PRIORITY_DEFAULT_IDLE, +[](gpointer data) -> gboolean {
      auto* widget = GTK_WIDGET(data);
      if (gtk_widget_get_parent(widget)) gtk_widget_unparent(widget);
      return G_SOURCE_REMOVE;
    }, g_object_ref(popover), +[](gpointer data) { g_object_unref(data); });
  }), new std::weak_ptr<Page>(weak), +[](gpointer data,GClosure*) { delete static_cast<std::weak_ptr<Page>*>(data); }, GConnectFlags(0));
  double x = json_object_get_double_member_with_default(hit,"x",0), y = json_object_get_double_member_with_default(hit,"y",0);
  if (json_object_get_boolean_member_with_default(hit,"pointer",false))
    gnosis_compositor_view_context_position(GNOSIS_COMPOSITOR_VIEW(s.surface),&x,&y);
  GdkRectangle rect = {int(std::clamp(x,0.0,double(gtk_widget_get_width(s.surface)))),
                       int(std::clamp(y,0.0,double(gtk_widget_get_height(s.surface)))),1,1};
  gtk_popover_set_pointing_to(GTK_POPOVER(popover),&rect);
  gtk_widget_set_halign(popover, GTK_ALIGN_START);
  auto* scroller = gtk_popover_get_child(GTK_POPOVER(popover));
  int natural_width = 0, natural_height = 0;
  // The popover itself is still hidden, so GTK would measure it as zero.
  // Its visible menu child supplies the actual natural content dimensions.
  gtk_widget_measure(scroller, GTK_ORIENTATION_HORIZONTAL, -1, nullptr, &natural_width, nullptr, nullptr);
  gtk_widget_measure(scroller, GTK_ORIENTATION_VERTICAL, natural_width, nullptr, &natural_height, nullptr, nullptr);
  graphene_point_t anchor = GRAPHENE_POINT_INIT(float(rect.x),float(rect.y)), root_anchor;
  auto* root = GTK_WIDGET(gtk_widget_get_root(s.surface));
  if (root && gtk_widget_compute_point(s.surface,root,&anchor,&root_anchor)) {
    const double above = root_anchor.y, below = gtk_widget_get_height(root) - root_anchor.y;
    gtk_popover_set_position(GTK_POPOVER(popover), natural_height > below && above > below ? GTK_POS_TOP : GTK_POS_BOTTOM);
  }
  // GTK's own scrolled menu remains natural-sized until the monitor limit.
  // Its popup layout asks the desktop compositor to flip vertically before
  // resizing. Wayland intentionally does not expose the window's screen origin.
  auto* native = gtk_widget_get_native(s.surface);
  auto* monitor = native ? gdk_display_get_monitor_at_surface(gtk_widget_get_display(s.surface),gtk_native_get_surface(native)) : nullptr;
  if (GTK_IS_SCROLLED_WINDOW(scroller) && monitor) {
    GdkRectangle geometry; gdk_monitor_get_geometry(monitor,&geometry);
    gtk_scrolled_window_set_max_content_height(GTK_SCROLLED_WINDOW(scroller),std::max(1,geometry.height));
    gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(scroller),GTK_POLICY_NEVER,GTK_POLICY_AUTOMATIC);
  }
  gnosis_compositor_view_release_buttons(GNOSIS_COMPOSITOR_VIEW(s.surface));
  gtk_popover_popup(GTK_POPOVER(popover));
}
void Page::set_visible(bool visible) {
  if (!visible) dismiss_http_retry();
  if (!visible) { dismiss_context_menu(); dismiss_script_dialog(true); dismiss_install_dialog(true); dismiss_authenticator(true); dismiss_passkey_provider(); }
  state_->mapped = visible;
  if (visible) update_portal_parent();
  if (state_->surface) gtk_widget_set_visible(state_->surface, visible);
  if (state_->display && state_->container) { if (visible) layout(); else XUnmapWindow(state_->display, state_->container); }
}
bool Page::devtools_open() const { return state_->devtools_window != nullptr; }
void Page::close_devtools() {
  auto& s = *state_;
  if (!s.devtools_target.empty() && s.process && !s.process->closing)
    s.process->command("Target.closeTarget", "{\"targetId\":" + quote(s.devtools_target) + "}", "");
  s.devtools_target.clear();
  if (s.devtools_window) {
    auto* window = s.devtools_window; s.devtools_window = nullptr;
    g_signal_handlers_disconnect_by_data(window, this);
    gtk_window_destroy(window);
  }
}
void Page::show_devtools() {
  auto& s = *state_;
  if (s.disposed || !s.process || !s.process->compositor || target.empty()) return;
  if (s.devtools_window) { gtk_window_present(s.devtools_window); return; }
  auto* window = GTK_WINDOW(adw_window_new());
  s.devtools_window = window;
  if (auto* root = gtk_widget_get_root(s.content); GTK_IS_WINDOW(root)) {
    gtk_window_set_transient_for(window, GTK_WINDOW(root));
    gtk_window_set_destroy_with_parent(window, true);
  }
  // Shell application windows must implement EphyEmbedContainer; a standalone
  // inspector is transient and must never enter that application window list.
  gtk_window_set_title(window, _("Developer Tools"));
  gtk_window_set_default_size(window, 1100, 750);
  auto* content = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
  gtk_box_append(GTK_BOX(content), adw_header_bar_new());
  auto* surface = gnosis_compositor_view_new_for_server(s.process->compositor, "__gnosis_devtools__");
  gtk_widget_set_hexpand(surface, true); gtk_widget_set_vexpand(surface, true);
  gtk_box_append(GTK_BOX(content), surface);
  adw_window_set_content(ADW_WINDOW(window), content);
  g_signal_connect(window, "close-request", G_CALLBACK(+[](GtkWindow*, gpointer data) -> gboolean {
    static_cast<Page*>(data)->close_devtools(); return true;
  }), this);
  gtk_window_present(window);
  s.process->command("Target.openDevTools", "{\"targetId\":" + quote(target) + "}", "",
    [weak = weak_from_this(), process = s.process](JsonObject* result, const std::string& error) {
      auto id = std::string(text(result, "targetId"));
      auto page = weak.lock();
      if (page && !page->is_closed() && page->state_->devtools_window) {
        if (!error.empty()) { g_warning("Developer Tools: %s", error.c_str()); page->close_devtools(); }
        else page->state_->devtools_target = id;
      } else if (!id.empty() && !process->closing)
        process->command("Target.closeTarget", "{\"targetId\":" + quote(id) + "}", "");
    });
}
void Page::close() {
  dismiss_http_retry();
  dismiss_passkey_provider();
  stop_find();
  const bool approved_close = close_is_approved();
  cancel_close();
  if (state_->process) std::erase_if(state_->process->frame_owners, [this](const auto& entry) {
    auto page = entry.second.lock(); return !page || page.get() == this;
  });
  if (!state_ || state_->disposed) return;
  auto& s = *state_;
  dismiss_script_dialog(true);
  dismiss_authenticator(true);
  dismiss_install_dialog(true);
  dismiss_translation();
  dismiss_vim_commands();
  set_vim_context(0);
  dismiss_password_manager();
  close_devtools();
  if (s.process && s.process->native_fd >= 0 && !target.empty()) {
    const auto message = "{\"kind\":\"authenticator-owner\",\"target\":" + quote(target) + ",\"enabled\":false}";
    if (send(s.process->native_fd, message.data(), message.size(), MSG_NOSIGNAL | MSG_DONTWAIT) != static_cast<ssize_t>(message.size()))
      s.process->close_native_ui();
  }
  if (s.process) s.process->forget_frames(this);
  s.disposed = true;
  for (auto& signal : s.theme_signals) {
    if (signal) g_signal_handler_disconnect(adw_style_manager_get_default(), signal);
    signal = 0;
  }
  s.theme_frames.clear(); s.theme_ready = false;
  if (s.process && s.native_tab >= 0) s.process->zoom_tabs.erase(s.native_tab);
  dismiss_context_menu();
  if (s.keys) { gtk_widget_remove_controller(s.content, s.keys); s.keys = nullptr; }
  changed = {}; closed = {}; failed = {}; navigation_started = {}; open_uri = {}; load_finished = {}; tls_changed = {}; fullscreen_changed = {}; zoom_changed = {}; favicon_changed = {}; event = {};
  popup_size_changed = {};
  if (s.timer) { g_source_remove(s.timer); s.timer = 0; }
  if (s.surface) {
    g_signal_handlers_disconnect_by_data(s.surface, this);
    // Action popups have a document observer and a short, owner-scoped life.
    // Never recycle their WebContents into another extension or browser tab.
    const bool keep = !approved_close && !s.extension_popup && !s.ephemeral && s.process && !s.process->closing && !s.native_destroyed && !target.empty() && !session.empty() && s.process->parked.empty();
    if (keep) {
      g_object_ref(s.surface);
      s.process->command("Page.stopLoading", "{}", session);
      s.process->command("Page.navigate", "{\"url\":\"about:blank\"}", session);
      s.process->parked.push_back({target, session, 0, s.surface});
    } else if (s.process && !s.process->closing && !s.native_destroyed && !target.empty()) {
      s.process->command("Target.closeTarget", "{\"targetId\":" + quote(target) + "}", "");
    }
    gtk_box_remove(GTK_BOX(s.content), s.surface); s.surface = nullptr;
    if (s.process) { s.process->pages.erase(s.token); s.process->targets.erase(target); s.process->sessions.erase(session); }
  if (s.ephemeral && s.process && !s.process->closing && !s.process->restarting) {
    bool live = false;
    for (auto& [token, weak] : s.process->pages)
      if (auto page = weak.lock(); page && !page->is_closed()) { live = true; break; }
    if (!live) {
      auto process = s.process;
      process->stop();
      retiring_private_processes.push_back(process);
      auto found = processes.find(true);
      if (found != processes.end() && found->second == process) processes.erase(found);
    }
  }
    return;
  }
  const bool park = s.process && !s.process->closing && !s.native_destroyed && s.display && s.child && !target.empty() && !session.empty();
  if (park) {
    XUnmapWindow(s.display, s.child);
    XReparentWindow(s.display, s.child, DefaultRootWindow(s.display), 0, 0);
    XDeleteProperty(s.display, s.child, XInternAtom(s.display, "WM_TRANSIENT_FOR", false));
    s.process->command("Page.stopLoading", "{}", session);
    s.process->command("Page.navigate", "{\"url\":\"about:blank\"}", session);
    // One reusable hidden page is sufficient to retain Chromium's app lifetime.
    // Close further parked pages so closing many tabs releases their renderers.
    if (s.process->parked.empty()) s.process->parked.push_back({target, session, s.child});
    else s.process->command("Target.closeTarget", "{\"targetId\":" + quote(target) + "}", "");
  } else if (s.process && !s.process->closing && !s.native_destroyed && !target.empty()) {
    s.process->command("Target.closeTarget", "{\"targetId\":" + quote(target) + "}", "");
  }
  if (s.process) { s.process->pages.erase(s.token); s.process->targets.erase(target); s.process->sessions.erase(session); }
  if (s.display) { if (s.container) { XUnmapWindow(s.display, s.container); XDestroyWindow(s.display, s.container); } XFlush(s.display); XCloseDisplay(s.display); s.display = nullptr; }
}
void Page::snapshot(std::function<void(GdkTexture*, const std::string&)> done, bool full_document) {
  auto weak = weak_from_this();
  const std::string expected = uri;
  Reply receive = [weak, expected, done = std::move(done)](JsonObject* result, const std::string& error) {
    auto self = weak.lock();
    if (!self || self->is_closed() || self->uri != expected) { done(nullptr, "The page changed during snapshot capture"); return; }
    if (!result) { done(nullptr, error); return; }
    gsize size = 0; guchar* data = g_base64_decode(text(result, "data"), &size);
    auto* bytes = g_bytes_new_take(data, size); GError* failure = nullptr; auto* texture = gdk_texture_new_from_bytes(bytes, &failure); g_bytes_unref(bytes);
    done(texture, failure ? failure->message : ""); g_clear_error(&failure); g_clear_object(&texture);
  };
  if (!full_document) { command("Page.captureScreenshot", "{\"format\":\"png\",\"captureBeyondViewport\":false}", std::move(receive)); return; }
  command("Page.getLayoutMetrics", "{}", [weak, expected, receive = std::move(receive)](JsonObject* result, const std::string& error) {
    auto self = weak.lock();
    if (!self || self->is_closed() || self->uri != expected) { receive(nullptr, "The page changed during snapshot capture"); return; }
    auto* viewport = object(result, "cssLayoutViewport"); auto* content = object(result, "cssContentSize");
    if (!viewport || !content) { receive(nullptr, error.empty() ? "The page has no snapshot dimensions" : error); return; }
    const double width = json_object_get_double_member_with_default(viewport, "clientWidth", 0);
    const double available = json_object_get_double_member_with_default(content, "height", 0);
    // Epiphany caches 650x540 snapshots and crops their top into overview
    // cards. A wide viewport alone is too short; include more document below
    // it without resizing/reflowing the live page or capturing its full length.
    const double height = std::min(available, width * 540.0 / 650.0);
    if (width <= 0 || height <= 0) { receive(nullptr, "Invalid snapshot dimensions"); return; }
    const double scale = std::min(1.0, 1300.0 / width);
    std::string params = "{\"format\":\"png\",\"captureBeyondViewport\":true,\"clip\":{\"x\":0,\"y\":0,\"width\":" +
      std::to_string(width) + ",\"height\":" + std::to_string(height) + ",\"scale\":" + std::to_string(scale) + "}}";
    self->command("Page.captureScreenshot", params, std::move(receive));
  });
}
bool Page::tls_info(GTlsCertificate** certificate, GTlsCertificateFlags* errors) const {
  if (certificate) *certificate = state_->certificate ? G_TLS_CERTIFICATE(g_object_ref(state_->certificate)) : nullptr;
  if (errors) *errors = state_->tls_errors;
  return state_->certificate != nullptr;
}
bool Page::tls_secure() const { return state_->certificate && state_->tls_errors == 0 && state_->security_state == "secure"; }
}
