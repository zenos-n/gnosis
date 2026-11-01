#include "browser.hpp"
#include "application.hpp"
#include "clipboard.hpp"
#include "page-messages.hpp"
#include "cursor.hpp"
#include "extension-tab-id.hpp"
#include "policy.hpp"
#include "ephy-engine-settings.h"
#include "ephy-engine-certificate.hpp"

#include "include/cef_parser.h"
#include "include/cef_task.h"
#include "include/wrapper/cef_helpers.h"
#include <algorithm>
#include <cmath>
#include <cstring>
#include <epoxy/egl.h>
#include <map>
#include <utility>

unsigned Browser::active_count = 0;
bool Browser::OnCertificateError(CefRefPtr<CefBrowser> source, cef_errorcode_t error, const CefString& uri,
                                 CefRefPtr<CefSSLInfo> info, CefRefPtr<CefCallback> callback) {
  if (owns(source) && !closing && host && info && uri == main_navigation_uri_) {
    if (ephy_engine_session_certificate_allowed(host->session, uri, info->GetX509Certificate())) {
      callback->Continue();
      return true;
    }
    certificate_failure_ = CertificateFailure{uri.ToString(), error, info, main_navigation_epoch_};
  }
  // Returning false delegates to Chromium's interstitial instead of Epiphany's page.
  callback->Cancel();
  return true;
}
namespace {
class ResponseTask final : public CefTask {
public:
  explicit ResponseTask(std::function<void()> callback) : callback_(std::move(callback)) {}
  void Execute() override { callback_(); }
private:
  std::function<void()> callback_;
  IMPLEMENT_REFCOUNTING(ResponseTask);
};
uint32_t modifiers(GdkModifierType state) {
  uint32_t result = 0;
  if (state & GDK_SHIFT_MASK)
    result |= EVENTFLAG_SHIFT_DOWN;
  if (state & GDK_CONTROL_MASK)
    result |= EVENTFLAG_CONTROL_DOWN;
  if (state & GDK_ALT_MASK)
    result |= EVENTFLAG_ALT_DOWN;
  if (state & GDK_SUPER_MASK)
    result |= EVENTFLAG_COMMAND_DOWN;
  if (state & GDK_LOCK_MASK)
    result |= EVENTFLAG_CAPS_LOCK_ON;
  if (state & GDK_BUTTON1_MASK)
    result |= EVENTFLAG_LEFT_MOUSE_BUTTON;
  if (state & GDK_BUTTON2_MASK)
    result |= EVENTFLAG_MIDDLE_MOUSE_BUTTON;
  if (state & GDK_BUTTON3_MASK)
    result |= EVENTFLAG_RIGHT_MOUSE_BUTTON;
  return result;
}
int windows_key(guint key) {
  if (key >= GDK_KEY_a && key <= GDK_KEY_z)
    return key - GDK_KEY_a + 'A';
  if (key >= GDK_KEY_A && key <= GDK_KEY_Z)
    return key;
  if (key >= GDK_KEY_0 && key <= GDK_KEY_9)
    return key;
  if (key >= GDK_KEY_F1 && key <= GDK_KEY_F24)
    return 0x70 + key - GDK_KEY_F1;
  switch (key) {
  case GDK_KEY_BackSpace:
    return 8;
  case GDK_KEY_Tab:
  case GDK_KEY_ISO_Left_Tab:
    return 9;
  case GDK_KEY_Return:
  case GDK_KEY_KP_Enter:
    return 13;
  case GDK_KEY_Shift_L:
  case GDK_KEY_Shift_R:
    return 16;
  case GDK_KEY_Control_L:
  case GDK_KEY_Control_R:
    return 17;
  case GDK_KEY_Alt_L:
  case GDK_KEY_Alt_R:
    return 18;
  case GDK_KEY_Escape:
    return 27;
  case GDK_KEY_space:
    return 32;
  case GDK_KEY_Page_Up:
    return 33;
  case GDK_KEY_Page_Down:
    return 34;
  case GDK_KEY_End:
    return 35;
  case GDK_KEY_Home:
    return 36;
  case GDK_KEY_Left:
    return 37;
  case GDK_KEY_Up:
    return 38;
  case GDK_KEY_Right:
    return 39;
  case GDK_KEY_Down:
    return 40;
  case GDK_KEY_Insert:
    return 45;
  case GDK_KEY_Delete:
    return 46;
  case GDK_KEY_Super_L:
    return 91;
  case GDK_KEY_Super_R:
    return 92;
  case GDK_KEY_semicolon:
  case GDK_KEY_colon:
    return 186;
  case GDK_KEY_equal:
  case GDK_KEY_plus:
    return 187;
  case GDK_KEY_comma:
  case GDK_KEY_less:
    return 188;
  case GDK_KEY_minus:
  case GDK_KEY_underscore:
    return 189;
  case GDK_KEY_period:
  case GDK_KEY_greater:
    return 190;
  case GDK_KEY_slash:
  case GDK_KEY_question:
    return 191;
  case GDK_KEY_grave:
  case GDK_KEY_asciitilde:
    return 192;
  case GDK_KEY_bracketleft:
  case GDK_KEY_braceleft:
    return 219;
  case GDK_KEY_backslash:
  case GDK_KEY_bar:
    return 220;
  case GDK_KEY_bracketright:
  case GDK_KEY_braceright:
    return 221;
  case GDK_KEY_apostrophe:
  case GDK_KEY_quotedbl:
    return 222;
  default:
    return gdk_keyval_to_unicode(key);
  }
}
CefKeyEvent key_event(guint keyval, guint keycode, GdkModifierType state, cef_key_event_type_t type) {
  CefKeyEvent event;
  event.type = type;
  event.windows_key_code = windows_key(keyval);
  event.native_key_code = keycode;
  event.modifiers = modifiers(state);
  event.is_system_key = (state & GDK_ALT_MASK) != 0;
  event.character = gdk_keyval_to_unicode(keyval);
  event.unmodified_character = gdk_keyval_to_unicode(gdk_keyval_to_lower(keyval));
  return event;
}
GLuint shader(GLenum type, const char* source) {
  GLuint id = glCreateShader(type);
  glShaderSource(id, 1, &source, nullptr);
  glCompileShader(id);
  GLint ok;
  glGetShaderiv(id, GL_COMPILE_STATUS, &ok);
  if (!ok) {
    char log[2048];
    glGetShaderInfoLog(id, sizeof log, nullptr, log);
    g_warning("Browser shader: %s", log);
  }
  return id;
}
} // namespace

Browser::Browser(std::string address, bool software) : url(std::move(address)), software_(software) {
  name = Policy::title(url);
  overlay_ = gtk_overlay_new();
  g_object_ref_sink(overlay_);
  gtk_widget_set_hexpand(overlay_, true);
  gtk_widget_set_vexpand(overlay_, true);
  g_object_set_data(G_OBJECT(overlay_), "browser", this);
  for (const auto* signal : {"notify::dark", "notify::accent-color"})
    g_signal_connect(adw_style_manager_get_default(), signal,
                     G_CALLBACK(+[](AdwStyleManager*, GParamSpec*, Browser* self) {
                       // Named GTK colors update after the style manager emits its notification.
                       g_idle_add_full(
                           G_PRIORITY_DEFAULT_IDLE,
                           +[](gpointer p) -> gboolean {
                             (*static_cast<CefRefPtr<Browser>*>(p))->refresh_theme();
                             return G_SOURCE_REMOVE;
                           },
                           new CefRefPtr<Browser>(self),
                           +[](gpointer p) { delete static_cast<CefRefPtr<Browser>*>(p); });
                     }),
                     this);
  if (GnosisBrave::enabled()) {
    area_ = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
    gtk_widget_set_focusable(area_, true); gtk_widget_set_hexpand(area_, true); gtk_widget_set_vexpand(area_, true);
    gtk_overlay_set_child(GTK_OVERLAY(overlay_), area_);
    im_ = gtk_im_multicontext_new();
    thumbnail_source_ = gtk_widget_paintable_new(overlay_);
    error_page_ = adw_status_page_new();
    auto* retry = gtk_button_new_with_label("Try Again");
    gtk_widget_add_css_class(retry, "pill");
    gtk_widget_set_halign(retry, GTK_ALIGN_CENTER);
    adw_status_page_set_child(ADW_STATUS_PAGE(error_page_), retry);
    g_signal_connect_swapped(retry, "clicked", G_CALLBACK(+[](Browser* self) { self->reload(); }), this);
    gtk_overlay_add_overlay(GTK_OVERLAY(overlay_), error_page_); gtk_widget_set_visible(error_page_, false);
    g_signal_connect(area_, "map", G_CALLBACK(+[](GtkWidget*, Browser* self) { if (!self->defer_start) self->start(); self->set_visible(true); }), this);
    g_signal_connect(area_, "unmap", G_CALLBACK(+[](GtkWidget*, Browser* self) { self->set_visible(false); }), this);
    return;
  }
  area_ = gtk_gl_area_new();
  gtk_gl_area_set_allowed_apis(GTK_GL_AREA(area_), GDK_GL_API_GLES);
  gtk_gl_area_set_required_version(GTK_GL_AREA(area_), 3, 0);
  gtk_gl_area_set_auto_render(GTK_GL_AREA(area_), false);
  gtk_widget_set_focusable(area_, true);
  gtk_widget_set_hexpand(area_, true);
  gtk_widget_set_vexpand(area_, true);
  gtk_overlay_set_child(GTK_OVERLAY(overlay_), area_);
  error_page_ = adw_status_page_new();
  adw_status_page_set_icon_name(ADW_STATUS_PAGE(error_page_), "network-offline-symbolic");
  GtkWidget* retry = gtk_button_new_with_label("Try Again");
  gtk_widget_add_css_class(retry, "suggested-action");
  gtk_widget_add_css_class(retry, "pill");
  gtk_widget_set_halign(retry, GTK_ALIGN_CENTER);
  adw_status_page_set_child(ADW_STATUS_PAGE(error_page_), retry);
  g_signal_connect_swapped(retry, "clicked", G_CALLBACK(+[](Browser* self) { self->reload(); }), this);
  gtk_overlay_add_overlay(GTK_OVERLAY(overlay_), error_page_);
  gtk_widget_set_visible(error_page_, false);
  g_signal_connect(area_, "realize", G_CALLBACK(+[](GtkGLArea*, Browser* self) { self->init_gl(); }), this);
  g_signal_connect(area_, "unrealize", G_CALLBACK(+[](GtkGLArea*, Browser* self) { self->clear_gl(); }),
                   this);
  g_signal_connect(area_, "render", G_CALLBACK(+[](GtkGLArea*, GdkGLContext*, Browser* self) -> gboolean {
                     self->render();
                     return true;
                   }),
                   this);
  g_signal_connect(area_, "resize", G_CALLBACK(+[](GtkGLArea*, int, int, Browser* self) {
                     if (self->browser_)
                       self->browser_->GetHost()->WasResized();
                   }),
                   this);
  g_signal_connect(area_, "notify::scale-factor", G_CALLBACK(+[](GObject*, GParamSpec*, Browser* self) {
                     if (self->browser_) {
                       self->browser_->GetHost()->NotifyScreenInfoChanged();
                       self->browser_->GetHost()->WasResized();
                     }
                   }),
                   this);
  g_signal_connect(area_, "map", G_CALLBACK(+[](GtkWidget*, Browser* self) {
                     if (!self->loaded && !self->defer_start)
                       self->start();
                     self->set_visible(true);
                   }),
                   this);
  g_signal_connect(area_, "unmap", G_CALLBACK(+[](GtkWidget*, Browser* self) {
                     // A chooser can keep its parent alive past window destruction.
                     self->cancel_file_dialogs();
                     self->set_visible(false);
                   }),
                   this);
  thumbnail_source_ = gtk_widget_paintable_new(overlay_);
  g_signal_connect(thumbnail_source_, "invalidate-contents",
                   G_CALLBACK(+[](GdkPaintable* source, Browser* self) {
                     if (!gtk_widget_get_mapped(self->overlay_) || self->closing)
                       return;
                     if (self->thumbnail_idle_)
                       return;
                     // Capturing synchronously from invalidate-contents can re-enter GTK's
                     // GLArea snapshot while its backing frame is being replaced. Cache only
                     // after that render has completed, not inside its invalidation callback.
                     self->thumbnail_idle_ = g_idle_add_full(
                         G_PRIORITY_LOW,
                         +[](gpointer p) -> gboolean {
                           auto* self = static_cast<Browser*>(p);
                           self->thumbnail_idle_ = 0;
                           if (!gtk_widget_get_mapped(self->overlay_) || self->closing)
                             return G_SOURCE_REMOVE;
                           GdkPaintable* image = gdk_paintable_get_current_image(self->thumbnail_source_);
                           if (gdk_paintable_get_intrinsic_width(image) > 0 &&
                               gdk_paintable_get_intrinsic_height(image) > 0)
                             g_set_object(&self->thumbnail_, image);
                           g_object_unref(image);
                           return G_SOURCE_REMOVE;
                         },
                         self, nullptr);
                   }),
                   this);
  setup_input();
}
Browser::~Browser() {
  if (brave_) { brave_->close(); brave_.reset(); }
  g_signal_handlers_disconnect_by_data(adw_style_manager_get_default(), this);
  g_clear_pointer(&account_pixels_, g_bytes_unref);
  if (thumbnail_idle_)
    g_source_remove(thumbnail_idle_);
  // A focused widget can emit controller signals while being removed. Detach
  // callbacks before releasing the IM context or the Browser they refer to.
  GListModel* controllers = gtk_widget_observe_controllers(area_);
  for (guint i = 0; i < g_list_model_get_n_items(controllers); ++i) {
    auto* controller = G_OBJECT(g_list_model_get_item(controllers, i));
    g_signal_handlers_disconnect_by_data(controller, this);
    g_object_unref(controller);
  }
  g_object_unref(controllers);
  g_signal_handlers_disconnect_by_data(area_, this);
  g_signal_handlers_disconnect_by_data(adw_status_page_get_child(ADW_STATUS_PAGE(error_page_)), this);
  g_signal_handlers_disconnect_by_data(thumbnail_source_, this);
  g_clear_object(&thumbnail_source_);
  g_clear_object(&thumbnail_);
  clear_gl();
  g_object_set_data(G_OBJECT(overlay_), "browser", nullptr);
  gtk_im_context_set_client_widget(im_, nullptr);
  g_object_unref(overlay_);
  g_object_unref(im_);
}
Window* Browser::owner() const {
  auto* root = gtk_widget_get_root(overlay_);
  if (host && root && GTK_IS_WINDOW(root)) {
    host->window = GTK_WINDOW(root);
    return host;
  }
  return nullptr;
}
void Browser::changed() {
  if (on_changed)
    on_changed();
  else if (owner())
    owner()->update();
}

void Browser::start() {
  if (GnosisBrave::enabled()) {
    // Mapping an existing tab must not navigate it again.
    if (closing) return;
    if (loaded) {
      if (brave_) brave_->start(); // An unparented view could not start until mapped.
      return;
    }
    if (!brave_) {
      brave_ = GnosisBrave::Page::create(area_, host && host->private_mode);
      brave_->set_extension_popup(is_extension_popup, extension_owner_tab_id);
      brave_->popup_size_changed = [this](int width, int height) {
        if (!closing && on_popup_size) on_popup_size(width, height);
      };
      brave_->changed = [this] { url = brave_->uri; name = brave_->title; loading = brave_->loading; progress = brave_->progress; changed(); };
      brave_->favicon_changed = [this](const std::string& page, const std::string& icon, GdkTexture* image) {
        if (!closing && on_favicon) on_favicon(page, icon, image);
      };
      brave_->navigation_started = [this](const std::string& value) { main_navigation_uri_ = value; if (on_load_start) on_load_start(value); };
      brave_->load_finished = [this](int status) {
        if (on_load_end) on_load_end(status);
        CefRefPtr<Browser> owner = this;
        brave_->snapshot([owner](GdkTexture* image, const std::string&) { if (!owner->closing && image) g_set_object(&owner->thumbnail_, GDK_PAINTABLE(image)); });
      };
      brave_->closed = [this] { if (on_closed) on_closed(); };
      brave_->tls_changed = [this] { if (on_security_changed) on_security_changed(); };
      brave_->fullscreen_changed = [this](bool value) { if (on_fullscreen_changed) on_fullscreen_changed(value); };
      brave_->open_uri = [this](const std::string& uri) {
        if (closing || !on_create_popup) return false;
        CefPopupFeatures features;
        auto child = on_create_popup(uri, features);
        if (!child || child->closing) return false;
        child->defer_start = false;
        child->navigate(uri);
        if (child->on_ready) child->on_ready();
        return true;
      };
      brave_->failed = [this](const std::string& error) {
        loaded = false; loading = false; progress = 0;
        show_error("Browser Process Stopped", error); changed();
      };
      changed();
    }
    brave_->navigate(url); loaded = true;
    return;
  }
  if (loaded || creating_ || waiting_context_ || waiting_extension_background_ || browser_ || closing || (owner() && (owner()->closing || owner()->destroyed)))
    return;
  if (is_extension_popup && !extension_background_ready_ && host && !host->private_mode &&
      url.starts_with("chrome-extension://")) {
    waiting_extension_background_ = true;
    CefRefPtr<Browser> self = this;
    const auto extension = url.substr(19, url.find('/', 19) - 19);
    host->app.extension_host->prepare_popup(extension,
      [self] { return !self->closing && self->host && !self->host->closing && !self->host->destroyed; },
      [self](const std::string& error) {
        self->waiting_extension_background_ = false;
        if (self->closing) return;
        if (!error.empty()) { self->show_error("Could Not Open Extension", error); return; }
        self->extension_background_ready_ = true;
        self->start();
      });
    return;
  }
  auto context = ephy_engine_session_context(host && host->session ? host->session : ephy_engine_session_get_default());
  if (!context) {
    waiting_context_ = true;
    CefRefPtr<Browser> self = this;
    CefPostDelayedTask(TID_UI, new ResponseTask([self] {
      self->waiting_context_ = false;
      self->start();
    }), 10);
    return;
  }
  loaded = true;
  creating_ = true;
  ++active_count;
  CefWindowInfo info;
  info.SetAsWindowless(0);
  info.shared_texture_enabled = !software_;
  CefBrowserSettings settings;
  settings.windowless_frame_rate = 60;
  settings.background_color = CefColorSetARGB(255, 255, 255, 255);
  // Extension bindings need their site selected when the web contents are created.
  creation_url_ = url.starts_with("chrome-extension://") ? url : "";
  if (!CefBrowserHost::CreateBrowser(info, this, creation_url_, settings, nullptr, context)) {
    creation_url_.clear();
    --active_count;
    creating_ = false;
    loaded = false;
    show_error("Could Not Start Browser", "Close this tab and try again.");
  }
}
void Browser::OnAfterCreated(CefRefPtr<CefBrowser> browser) {
  CefRefPtr<Browser> keep_alive = this;
  if (browser_id_) {
    // A client belongs to exactly one native tab and one Chromium browser.
    // Reject unexpected extra creations without replacing its live browser.
    if (browser_id_ != browser->GetIdentifier())
      browser->GetHost()->CloseBrowser(true);
    return;
  }
  const bool requested = creating_;
  creating_ = false;
  browser_ = browser;
  browser_id_ = browser->GetIdentifier();
  if (popup_opener_) {
    auto* opener = popup_opener_;
    popup_opener_ = nullptr;
    opener->pending_popups_.erase(popup_id_);
  }
  cookie_filter_ = ephy_engine_session_cookie_filter(host && host->session ? host->session : ephy_engine_session_get_default());
  session_request_handler_ = ephy_engine_session_request_handler(host && host->session ? host->session : ephy_engine_session_get_default());
  loaded = true;
  if (!requested) ++active_count;
  if (host) {
    held_app_ = G_APPLICATION(g_object_ref(host->app.gtk_app));
    g_application_hold(held_app_);
  }
  if (host && !host->private_mode) {
    attached_host_ = host->app.extension_host;
    attached_host_->attach(browser);
  }
  if (closing) {
    browser_->GetHost()->CloseBrowser(true);
    return;
  }
  browser_->GetHost()->WasResized();
  set_visible(gtk_widget_get_mapped(area_));
  refresh_theme();
  set_permissions();
  changed();
  if (closing) return;
  // Ordinary pages start blank for preference setup; native extension pages already
  // have their initial URL. Chromium-owned popups retain their opener navigation.
  auto prepare = prepare_navigation;
  if (prepare) prepare();
  if (closing) return;
  const bool supplied_initial_url = requested && !creation_url_.empty() && creation_url_ == url;
  creation_url_.clear();
  const bool navigate = !native_popup_child_ &&
                        (requested || !pending_navigation_.empty()) && !supplied_initial_url;
  pending_navigation_.clear();
  if (navigate) browser_->GetMainFrame()->LoadURL(url);
  auto ready = on_ready;
  if (ready) ready();
}
bool Browser::owns(CefRefPtr<CefBrowser> source) const {
  return source && browser_id_ && source->GetIdentifier() == browser_id_;
}
void Browser::OnBeforeClose(CefRefPtr<CefBrowser> source) {
  if (!owns(source))
    return;
  closing = true;
  certificate_failure_.reset();
  cancel_file_dialogs();
  cancel_permissions();
  cancel_authentication();
  cancel_pending_popups();
  ephy_engine_download_browser_closed(browser_id_);
  CefRefPtr<Browser> keep_alive = this;
  Window* win = owner();
  if (attached_host_) { attached_host_->detach(browser_id_); attached_host_ = nullptr; }
  browser_ = nullptr;
  browser_id_ = 0;
  loaded = false;
  creating_ = false;
  --active_count;
  auto closed = on_closed;
  if (closed)
    closed();
  else if (win)
    win->browser_closed(this);
  if (held_app_) { g_application_release(held_app_); g_clear_object(&held_app_); }
}
void Browser::detach_callbacks() {
  page_message_context.clear();
  cancel_authentication();
  on_authentication = nullptr;
  on_client_certificate = nullptr;
  on_permission = nullptr;
  on_page_message = nullptr;
  on_external_uri = nullptr;
  on_webstore_install_blocked = nullptr;
  on_changed = nullptr;
  on_popup_size = nullptr;
  on_security_changed = nullptr;
  on_fullscreen_changed = nullptr;
  on_favicon = nullptr;
  on_closed = nullptr;
  on_close_cancelled = nullptr;
  on_create_popup = nullptr;
  on_ready = nullptr;
  on_contents_bounds_change = nullptr;
  on_load_end = nullptr;
  on_load_error = nullptr;
  on_tls_error = nullptr;
  certificate_failure_.reset();
  on_load_start = nullptr;
  prepare_navigation = nullptr;
  on_response = nullptr;
  on_resource_complete = nullptr;
}
void Browser::cancel_pending_popups() {
  auto pending = std::move(pending_popups_);
  pending_popups_.clear();
  for (auto& [id, popup] : pending) {
    popup->popup_opener_ = nullptr;
    if (popup->creating_) {
      popup->creating_ = false;
      --active_count;
    }
    popup->loaded = false;
    popup->close();
  }
}
void Browser::OnBeforePopupAborted(CefRefPtr<CefBrowser> source, int popup_id) {
  if (!owns(source)) return;
  auto found = pending_popups_.find(popup_id);
  if (found == pending_popups_.end()) return;
  auto popup = found->second;
  pending_popups_.erase(found);
  popup->popup_opener_ = nullptr;
  if (popup->creating_) {
    popup->creating_ = false;
    --active_count;
  }
  popup->loaded = false;
  popup->close();
}
void Browser::close() {
  if (brave_) { closing = true; brave_->close(); if (on_closed) on_closed(); return; }
  certificate_failure_.reset();
  cancel_file_dialogs();
  cancel_permissions();
  cancel_authentication();
  if (closing)
    return;
  gtk_im_context_focus_out(im_);
  gtk_im_context_set_client_widget(im_, nullptr);
  closing = true;
  if (browser_ && downloads_.empty())
    browser_->GetHost()->CloseBrowser(false);
  else if (!loaded) {
    if (on_closed)
      on_closed();
    else if (owner())
      owner()->browser_closed(this);
  }
}
void Browser::shutdown() {
  if (brave_) { closing = true; brave_->close(); detach_callbacks(); return; }
  detach_callbacks();
  cancel_file_dialogs();
  cancel_permissions();
  cancel_authentication();
  closing = true;
  if (browser_) browser_->GetHost()->CloseBrowser(true);
}
void Browser::navigate(const std::string& address) {
  if (GnosisBrave::enabled()) { url = address; main_navigation_uri_ = address; if (brave_) brave_->navigate(address); else start(); return; }
  if (address == url || address.substr(0, address.find('#')) != url.substr(0, url.find('#')))
    page_message_context.clear();
  cancel_authentication();
  // Local documents can commit without Chromium's OnBeforeBrowse callback.
  certificate_failure_.reset();
  ++main_navigation_epoch_;
  main_navigation_uri_ = address;
  loading_document_frame_.clear();
  url = address;
  gtk_widget_set_visible(error_page_, false);
  if (browser_)
    browser_->GetMainFrame()->LoadURL(address);
  else if (loaded || creating_)
    pending_navigation_ = address;
  else
    start();
}
void Browser::reload() {
  if (GnosisBrave::enabled()) {
    gtk_widget_set_visible(error_page_, false);
    if (brave_ && !brave_->is_closed()) brave_->reload();
    else { brave_.reset(); loaded = false; start(); }
    return;
  }
  page_message_context.clear();
  gtk_widget_set_visible(error_page_, false);
  if (browser_)
    browser_->Reload();
  else if (loaded || creating_)
    pending_navigation_ = url;
  else
    start();
}
void Browser::back() {
  if (brave_) { brave_->back(); return; }
  if (browser_)
    browser_->GoBack();
}
void Browser::forward() {
  if (brave_) { brave_->forward(); return; }
  if (browser_)
    browser_->GoForward();
}
void Browser::set_visible(bool visible) {
  if (brave_) { brave_->set_visible(visible); return; }
  if (!visible) cancel_authentication();
  if (browser_) {
    browser_->GetHost()->WasHidden(!visible);
    if (visible)
      browser_->GetHost()->Invalidate(PET_VIEW);
  }
}
void Browser::refresh_theme() {}
GdkPaintable* Browser::thumbnail() {
  // GTK retains the last full-resolution render node, including its GL texture.
  // Snapshotting it is constant-time; no pixel readback or downsampling occurs
  // on the tab activation path. It also preserves the previous hidden page.
  if (thumbnail_)
    return GDK_PAINTABLE(g_object_ref(thumbnail_));
  return nullptr;
}
GdkTexture* Browser::snapshot_texture() {
  if (brave_) return thumbnail_ && GDK_IS_TEXTURE(thumbnail_) ? GDK_TEXTURE(g_object_ref(thumbnail_)) : nullptr;
  // An explicit saved snapshot must use the latest CEF frame, not the GTK
  // thumbnail node, whose low-priority refresh may still be pending.
  struct RestoreContext {
    GdkGLContext* previous = gdk_gl_context_get_current();
    RestoreContext() { if (previous) g_object_ref(previous); }
    ~RestoreContext() {
      if (previous) {
        gdk_gl_context_make_current(previous);
        g_object_unref(previous);
      } else {
        gdk_gl_context_clear_current();
      }
    }
  } restore_context;
  if (closing || !texture_ || texture_width_ <= 0 || texture_height_ <= 0 || !gl_ready())
    return nullptr;
  const gsize stride = static_cast<gsize>(texture_width_) * 4;
  if (static_cast<gsize>(texture_height_) > G_MAXSIZE / stride)
    return nullptr;
  const gsize size = stride * texture_height_;
  auto* pixels = static_cast<guint8*>(g_try_malloc(size));
  if (!pixels)
    return nullptr;

  GLint previous_read, previous_draw, previous_read_buffer, pack_buffer;
  GLint alignment, row_length, skip_rows, skip_pixels;
  glGetIntegerv(GL_READ_FRAMEBUFFER_BINDING, &previous_read);
  glGetIntegerv(GL_DRAW_FRAMEBUFFER_BINDING, &previous_draw);
  glGetIntegerv(GL_READ_BUFFER, &previous_read_buffer);
  glGetIntegerv(GL_PIXEL_PACK_BUFFER_BINDING, &pack_buffer);
  glGetIntegerv(GL_PACK_ALIGNMENT, &alignment);
  glGetIntegerv(GL_PACK_ROW_LENGTH, &row_length);
  glGetIntegerv(GL_PACK_SKIP_ROWS, &skip_rows);
  glGetIntegerv(GL_PACK_SKIP_PIXELS, &skip_pixels);
  GLuint framebuffer;
  glGenFramebuffers(1, &framebuffer);
  glBindFramebuffer(GL_READ_FRAMEBUFFER, framebuffer);
  glFramebufferTexture2D(GL_READ_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, texture_, 0);
  const bool complete = glCheckFramebufferStatus(GL_READ_FRAMEBUFFER) == GL_FRAMEBUFFER_COMPLETE;
  GLenum error = GL_NO_ERROR;
  if (complete) {
    glReadBuffer(GL_COLOR_ATTACHMENT0);
    glBindBuffer(GL_PIXEL_PACK_BUFFER, 0);
    glPixelStorei(GL_PACK_ALIGNMENT, 4);
    glPixelStorei(GL_PACK_ROW_LENGTH, 0);
    glPixelStorei(GL_PACK_SKIP_ROWS, 0);
    glPixelStorei(GL_PACK_SKIP_PIXELS, 0);
    glReadPixels(0, 0, texture_width_, texture_height_, GL_RGBA, GL_UNSIGNED_BYTE, pixels);
    error = glGetError();
  }
  glBindBuffer(GL_PIXEL_PACK_BUFFER, pack_buffer);
  glPixelStorei(GL_PACK_ALIGNMENT, alignment);
  glPixelStorei(GL_PACK_ROW_LENGTH, row_length);
  glPixelStorei(GL_PACK_SKIP_ROWS, skip_rows);
  glPixelStorei(GL_PACK_SKIP_PIXELS, skip_pixels);
  glBindFramebuffer(GL_READ_FRAMEBUFFER, previous_read);
  glReadBuffer(previous_read_buffer);
  glBindFramebuffer(GL_DRAW_FRAMEBUFFER, previous_draw);
  glDeleteFramebuffers(1, &framebuffer);
  if (!complete || error != GL_NO_ERROR) {
    g_free(pixels);
    return nullptr;
  }

  // CEF's first image row is stored at texture y=0. The display shader flips
  // texture coordinates, so readback is already in GDK's top-to-bottom order.
  GBytes* bytes = g_bytes_new_take(pixels, size);
  const auto format = texture_bgra_ ? GDK_MEMORY_B8G8R8A8_PREMULTIPLIED : GDK_MEMORY_R8G8B8A8_PREMULTIPLIED;
  GdkTexture* snapshot = gdk_memory_texture_new(texture_width_, texture_height_, format, bytes, stride);
  g_bytes_unref(bytes);
  return snapshot;
}
void Browser::set_permissions() {}
void Browser::GetViewRect(CefRefPtr<CefBrowser>, CefRect& rect) {
  rect = CefRect(0, 0, std::max(1, gtk_widget_get_width(area_)), std::max(1, gtk_widget_get_height(area_)));
}
bool Browser::GetScreenInfo(CefRefPtr<CefBrowser> browser, CefScreenInfo& info) {
  CefRect rect;
  GetViewRect(browser, rect);
  info.device_scale_factor = gtk_widget_get_scale_factor(area_);
  info.depth = 32;
  info.depth_per_component = 8;
  info.rect = info.available_rect = rect;
  return true;
}
bool Browser::gl_ready() {
  if (!gtk_widget_get_realized(area_))
    return false;
  gtk_gl_area_make_current(GTK_GL_AREA(area_));
  return !gtk_gl_area_get_error(GTK_GL_AREA(area_));
}
void Browser::init_gl() {
  if (!gl_ready()) {
    const GError* error = gtk_gl_area_get_error(GTK_GL_AREA(area_));
    show_error("Graphics Not Available", error ? error->message : "An OpenGL ES 3 context is required.");
    return;
  }
  const char* vs =
      "#version 300 es\nprecision highp float; out vec2 uv; void main(){vec2 "
      "p=vec2((gl_VertexID<<1)&2,gl_VertexID&2); uv=vec2(p.x,1.0-p.y); gl_Position=vec4(p*2.0-1.0,0,1);}";
  const char* fs = "#version 300 es\nprecision highp float; in vec2 uv; uniform sampler2D frame; uniform "
                   "bool bgra; out vec4 color; void main(){vec4 c=texture(frame,uv);color=bgra?c.bgra:c;}";
  GLuint vertex = shader(GL_VERTEX_SHADER, vs), fragment = shader(GL_FRAGMENT_SHADER, fs);
  program_ = glCreateProgram();
  glAttachShader(program_, vertex);
  glAttachShader(program_, fragment);
  glLinkProgram(program_);
  glDeleteShader(vertex);
  glDeleteShader(fragment);
  glGenVertexArrays(1, &vao_);
  if (browser_)
    browser_->GetHost()->Invalidate(PET_VIEW);
}
void Browser::clear_gl() {
  if (gl_ready()) {
    if (texture_)
      glDeleteTextures(1, &texture_);
    if (popup_texture_)
      glDeleteTextures(1, &popup_texture_);
    if (program_)
      glDeleteProgram(program_);
    if (vao_)
      glDeleteVertexArrays(1, &vao_);
  }
  texture_ = popup_texture_ = program_ = vao_ = 0;
  texture_width_ = texture_height_ = 0;
}
void Browser::render() {
  if (!gl_ready() || !program_)
    return;
  const GLenum toggles[] = {GL_SCISSOR_TEST, GL_DEPTH_TEST, GL_STENCIL_TEST,
                            GL_CULL_FACE,    GL_BLEND,      GL_RASTERIZER_DISCARD};
  GLboolean enabled[6], color_mask[4];
  GLint old_viewport[4], old_program, old_vao, old_active, old_texture;
  for (size_t i = 0; i < 6; ++i) {
    enabled[i] = glIsEnabled(toggles[i]);
    glDisable(toggles[i]);
  }
  glGetBooleanv(GL_COLOR_WRITEMASK, color_mask);
  glGetIntegerv(GL_VIEWPORT, old_viewport);
  glGetIntegerv(GL_CURRENT_PROGRAM, &old_program);
  glGetIntegerv(GL_VERTEX_ARRAY_BINDING, &old_vao);
  glGetIntegerv(GL_ACTIVE_TEXTURE, &old_active);
  glActiveTexture(GL_TEXTURE0);
  glGetIntegerv(GL_TEXTURE_BINDING_2D, &old_texture);
  glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
  const int scale = gtk_widget_get_scale_factor(area_);
  const int width = gtk_widget_get_width(area_) * scale, height = gtk_widget_get_height(area_) * scale;
  glViewport(0, 0, width, height);
  glClearColor(1, 1, 1, 1);
  glClear(GL_COLOR_BUFFER_BIT);
  glUseProgram(program_);
  glBindVertexArray(vao_);
  glActiveTexture(GL_TEXTURE0);
  glUniform1i(glGetUniformLocation(program_, "frame"), 0);
  if (texture_) {
    glBindTexture(GL_TEXTURE_2D, texture_);
    glUniform1i(glGetUniformLocation(program_, "bgra"), texture_bgra_);
    glDrawArrays(GL_TRIANGLES, 0, 3);
  }
  if (popup_visible_ && popup_texture_) {
    glEnable(GL_BLEND);
    glBlendFunc(GL_ONE, GL_ONE_MINUS_SRC_ALPHA);
    glViewport(popup_rect_.x * scale, height - (popup_rect_.y + popup_rect_.height) * scale,
               popup_rect_.width * scale, popup_rect_.height * scale);
    glBindTexture(GL_TEXTURE_2D, popup_texture_);
    glUniform1i(glGetUniformLocation(program_, "bgra"), popup_bgra_);
    glDrawArrays(GL_TRIANGLES, 0, 3);
    glDisable(GL_BLEND);
  }
  glBindTexture(GL_TEXTURE_2D, old_texture);
  glActiveTexture(old_active);
  glBindVertexArray(old_vao);
  glUseProgram(old_program);
  glViewport(old_viewport[0], old_viewport[1], old_viewport[2], old_viewport[3]);
  glColorMask(color_mask[0], color_mask[1], color_mask[2], color_mask[3]);
  for (size_t i = 0; i < 6; ++i) {
    if (enabled[i])
      glEnable(toggles[i]);
    else
      glDisable(toggles[i]);
  }
}
void Browser::upload(PaintElementType type, const void* pixels, int width, int height) {
  if (!gl_ready() || width <= 0 || height <= 0)
    return;
  GLuint& texture = type == PET_POPUP ? popup_texture_ : texture_;
  if (!texture)
    glGenTextures(1, &texture);
  glBindTexture(GL_TEXTURE_2D, texture);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
  GLint unpack_buffer, row_length, skip_rows, skip_pixels, alignment;
  glGetIntegerv(GL_PIXEL_UNPACK_BUFFER_BINDING, &unpack_buffer);
  glGetIntegerv(GL_UNPACK_ROW_LENGTH, &row_length);
  glGetIntegerv(GL_UNPACK_SKIP_ROWS, &skip_rows);
  glGetIntegerv(GL_UNPACK_SKIP_PIXELS, &skip_pixels);
  glGetIntegerv(GL_UNPACK_ALIGNMENT, &alignment);
  glBindBuffer(GL_PIXEL_UNPACK_BUFFER, 0);
  glPixelStorei(GL_UNPACK_ROW_LENGTH, 0);
  glPixelStorei(GL_UNPACK_SKIP_ROWS, 0);
  glPixelStorei(GL_UNPACK_SKIP_PIXELS, 0);
  glPixelStorei(GL_UNPACK_ALIGNMENT, 4);
  if (type == PET_VIEW && width == texture_width_ && height == texture_height_)
    glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, width, height, GL_RGBA, GL_UNSIGNED_BYTE, pixels);
  else
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, width, height, 0, GL_RGBA, GL_UNSIGNED_BYTE, pixels);
  glBindBuffer(GL_PIXEL_UNPACK_BUFFER, unpack_buffer);
  glPixelStorei(GL_UNPACK_ROW_LENGTH, row_length);
  glPixelStorei(GL_UNPACK_SKIP_ROWS, skip_rows);
  glPixelStorei(GL_UNPACK_SKIP_PIXELS, skip_pixels);
  glPixelStorei(GL_UNPACK_ALIGNMENT, alignment);
  if (type == PET_VIEW) {
    texture_width_ = width;
    texture_height_ = height;
    texture_bgra_ = true;
  } else
    popup_bgra_ = true;
  gtk_gl_area_queue_render(GTK_GL_AREA(area_));
}
void Browser::OnPaint(CefRefPtr<CefBrowser> source, PaintElementType type, const RectList&,
                      const void* buffer, int width, int height) {
  if (owns(source))
    upload(type, buffer, width, height);
}
void Browser::OnAcceleratedPaint(CefRefPtr<CefBrowser> source, PaintElementType type, const RectList&,
                                 const CefAcceleratedPaintInfo& info) {
  if (!owns(source))
    return;
  if (!gl_ready())
    return;
  const EGLDisplay display = eglGetCurrentDisplay();
  if (display == EGL_NO_DISPLAY || !epoxy_has_egl_extension(display, "EGL_EXT_image_dma_buf_import")) {
    show_error("Graphics Driver Not Supported", "GPU frame sharing is unavailable. Launch with "
                                                "--software-rendering to use the compatibility renderer.");
    return;
  }
  const int width = info.extra.coded_size.width, height = info.extra.coded_size.height;
  if (width <= 0 || height <= 0 || info.plane_count < 1 || info.plane_count > 4)
    return;
  // FourCC AR24 (BGRA bytes) / AB24 (RGBA bytes), little-endian.
  const EGLint format = info.format == CEF_COLOR_TYPE_BGRA_8888 ? 0x34325241 : 0x34324241;
  std::vector<EGLint> attrs = {EGL_WIDTH, width, EGL_HEIGHT, height, EGL_LINUX_DRM_FOURCC_EXT, format};
  const EGLint fd[] = {EGL_DMA_BUF_PLANE0_FD_EXT, EGL_DMA_BUF_PLANE1_FD_EXT, EGL_DMA_BUF_PLANE2_FD_EXT,
                       EGL_DMA_BUF_PLANE3_FD_EXT};
  const EGLint offset[] = {EGL_DMA_BUF_PLANE0_OFFSET_EXT, EGL_DMA_BUF_PLANE1_OFFSET_EXT,
                           EGL_DMA_BUF_PLANE2_OFFSET_EXT, EGL_DMA_BUF_PLANE3_OFFSET_EXT};
  const EGLint pitch[] = {EGL_DMA_BUF_PLANE0_PITCH_EXT, EGL_DMA_BUF_PLANE1_PITCH_EXT,
                          EGL_DMA_BUF_PLANE2_PITCH_EXT, EGL_DMA_BUF_PLANE3_PITCH_EXT};
  const EGLint low[] = {EGL_DMA_BUF_PLANE0_MODIFIER_LO_EXT, EGL_DMA_BUF_PLANE1_MODIFIER_LO_EXT,
                        EGL_DMA_BUF_PLANE2_MODIFIER_LO_EXT, EGL_DMA_BUF_PLANE3_MODIFIER_LO_EXT};
  const EGLint high[] = {EGL_DMA_BUF_PLANE0_MODIFIER_HI_EXT, EGL_DMA_BUF_PLANE1_MODIFIER_HI_EXT,
                         EGL_DMA_BUF_PLANE2_MODIFIER_HI_EXT, EGL_DMA_BUF_PLANE3_MODIFIER_HI_EXT};
  const bool use_modifiers = epoxy_has_egl_extension(display, "EGL_EXT_image_dma_buf_import_modifiers");
  for (int i = 0; i < info.plane_count; ++i) {
    attrs.insert(attrs.end(),
                 {fd[i], info.planes[i].fd, offset[i], static_cast<EGLint>(info.planes[i].offset), pitch[i],
                  static_cast<EGLint>(info.planes[i].stride)});
    if (use_modifiers)
      attrs.insert(attrs.end(), {low[i], static_cast<EGLint>(info.modifier & 0xffffffff), high[i],
                                 static_cast<EGLint>(info.modifier >> 32)});
  }
  attrs.push_back(EGL_NONE);
  EGLImage image = eglCreateImageKHR(display, EGL_NO_CONTEXT, EGL_LINUX_DMA_BUF_EXT, nullptr, attrs.data());
  if (image == EGL_NO_IMAGE_KHR) {
    show_error("Could Not Share GPU Frame", "Try the compatibility renderer with --software-rendering.");
    return;
  }
  GLint previous_read, previous_draw;
  glGetIntegerv(GL_READ_FRAMEBUFFER_BINDING, &previous_read);
  glGetIntegerv(GL_DRAW_FRAMEBUFFER_BINDING, &previous_draw);
  GLuint imported, read_fbo, write_fbo;
  glGenTextures(1, &imported);
  glBindTexture(GL_TEXTURE_2D, imported);
  glEGLImageTargetTexture2DOES(GL_TEXTURE_2D, image);
  glGenFramebuffers(1, &read_fbo);
  glBindFramebuffer(GL_READ_FRAMEBUFFER, read_fbo);
  glFramebufferTexture2D(GL_READ_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, imported, 0);
  GLuint& texture = type == PET_POPUP ? popup_texture_ : texture_;
  if (!texture)
    glGenTextures(1, &texture);
  glBindTexture(GL_TEXTURE_2D, texture);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
  if (type == PET_POPUP || width != texture_width_ || height != texture_height_)
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, width, height, 0, GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
  glGenFramebuffers(1, &write_fbo);
  glBindFramebuffer(GL_DRAW_FRAMEBUFFER, write_fbo);
  glFramebufferTexture2D(GL_DRAW_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, texture, 0);
  if (glCheckFramebufferStatus(GL_READ_FRAMEBUFFER) == GL_FRAMEBUFFER_COMPLETE &&
      glCheckFramebufferStatus(GL_DRAW_FRAMEBUFFER) == GL_FRAMEBUFFER_COMPLETE) {
    const bool clipped = glIsEnabled(GL_SCISSOR_TEST);
    glDisable(GL_SCISSOR_TEST);
    glBlitFramebuffer(0, 0, width, height, 0, 0, width, height, GL_COLOR_BUFFER_BIT, GL_NEAREST);
    if (clipped)
      glEnable(GL_SCISSOR_TEST);
    // CEF reuses the source buffer once this callback returns. Finish the GPU copy first.
    glFinish();
    gpu_frames = true;
    if (type == PET_VIEW) {
      texture_width_ = width;
      texture_height_ = height;
      texture_bgra_ = false;
    } else
      popup_bgra_ = false;
    gtk_gl_area_queue_render(GTK_GL_AREA(area_));
  }
  glBindFramebuffer(GL_READ_FRAMEBUFFER, previous_read);
  glBindFramebuffer(GL_DRAW_FRAMEBUFFER, previous_draw);
  glDeleteFramebuffers(1, &read_fbo);
  glDeleteFramebuffers(1, &write_fbo);
  glDeleteTextures(1, &imported);
  eglDestroyImageKHR(display, image);
}
void Browser::OnPopupShow(CefRefPtr<CefBrowser> source, bool show) {
  if (!owns(source))
    return;
  popup_visible_ = show;
  gtk_gl_area_queue_render(GTK_GL_AREA(area_));
}
void Browser::show_error(const std::string& title, const std::string& description) {
  adw_status_page_set_title(ADW_STATUS_PAGE(error_page_), title.c_str());
  adw_status_page_set_description(ADW_STATUS_PAGE(error_page_), description.c_str());
  gtk_widget_set_visible(error_page_, true);
}
void Browser::setup_input() {
  im_ = gtk_im_multicontext_new();
  g_object_set_data(G_OBJECT(area_), "browser-im", im_);
  gtk_im_context_set_client_widget(im_, area_);
  g_signal_connect(im_, "commit", G_CALLBACK(+[](GtkIMContext*, const char* text, Browser* self) {
                     if (!self->browser_)
                       return;
                     if (self->im_space_event_ && !g_strcmp0(text, " ")) {
                       // A literal key needs CHAR for Chromium's Space activation, not an IM insertion.
                       self->browser_->GetHost()->SendKeyEvent(*self->im_space_event_);
                       self->im_space_event_.reset();
                     } else {
                       self->im_space_event_.reset();
                       self->browser_->GetHost()->ImeCommitText(text, CefRange(UINT32_MAX, UINT32_MAX), 0);
                     }
                   }),
                   this);
  g_signal_connect(im_, "preedit-changed", G_CALLBACK(+[](GtkIMContext* im, Browser* self) {
                     if (!self->browser_)
                       return;
                     char* text;
                     int cursor;
                     PangoAttrList* attrs;
                     gtk_im_context_get_preedit_string(im, &text, &attrs, &cursor);
                     self->im_preedit_active_ = text && *text;
                     if (self->im_preedit_active_)
                       self->im_space_event_.reset();
                     std::u16string value = CefString(text).ToString16();
                     CefCompositionUnderline underline;
                     underline.range = CefRange(0, value.size());
                     underline.color = CefColorSetARGB(255, 0, 0, 0);
                     underline.thick = false;
                     self->browser_->GetHost()->ImeSetComposition(
                         text, {underline}, CefRange(UINT32_MAX, UINT32_MAX), CefRange(cursor, cursor));
                     g_free(text);
                     pango_attr_list_unref(attrs);
                   }),
                   this);
  GtkEventController* focus = gtk_event_controller_focus_new();
  g_signal_connect(focus, "enter", G_CALLBACK(+[](GtkEventControllerFocus*, Browser* self) {
                     if (self->closing)
                       return;
                     gtk_im_context_set_client_widget(self->im_, self->area_);
                     gtk_im_context_focus_in(self->im_);
                     if (self->browser_)
                       self->browser_->GetHost()->SetFocus(true);
                   }),
                   this);
  g_signal_connect(focus, "leave", G_CALLBACK(+[](GtkEventControllerFocus*, Browser* self) {
                     gtk_im_context_focus_out(self->im_);
                     if (self->browser_)
                       self->browser_->GetHost()->SetFocus(false);
                   }),
                   this);
  gtk_widget_add_controller(area_, focus);
  GtkEventController* motion = gtk_event_controller_motion_new();
  g_signal_connect(
      motion, "motion", G_CALLBACK(+[](GtkEventControllerMotion* c, double x, double y, Browser* self) {
        self->mouse_x_ = x;
        self->mouse_y_ = y;
        if (self->browser_)
          self->browser_->GetHost()->SendMouseMoveEvent(
              self->mouse(gtk_event_controller_get_current_event_state(GTK_EVENT_CONTROLLER(c))), false);
      }),
      this);
  g_signal_connect(motion, "leave", G_CALLBACK(+[](GtkEventControllerMotion* c, Browser* self) {
                     if (self->browser_)
                       self->browser_->GetHost()->SendMouseMoveEvent(
                           self->mouse(gtk_event_controller_get_current_event_state(GTK_EVENT_CONTROLLER(c))),
                           true);
                   }),
                   this);
  gtk_widget_add_controller(area_, motion);
  GtkGesture* click = gtk_gesture_click_new();
  gtk_gesture_single_set_button(GTK_GESTURE_SINGLE(click), 0);
  g_signal_connect(click, "pressed",
                   G_CALLBACK(+[](GtkGestureClick* c, int n, double x, double y, Browser* self) {
                     self->activate_clipboard();
                     self->mouse_x_ = x;
                     self->mouse_y_ = y;
                     const guint button = gtk_gesture_single_get_current_button(GTK_GESTURE_SINGLE(c));
                     self->drag_button_ = button;
                     if (!self->browser_)
                       return;
                     if (button == 8) {
                       self->back();
                       return;
                     }
                     if (button == 9) {
                       self->forward();
                       return;
                     }
                     self->browser_->GetHost()->SendMouseClickEvent(
                         self->mouse(gtk_event_controller_get_current_event_state(GTK_EVENT_CONTROLLER(c))),
                         button == 2   ? MBT_MIDDLE
                         : button == 3 ? MBT_RIGHT
                                       : MBT_LEFT,
                         false, std::min(n, 3));
                   }),
                   this);
  g_signal_connect(click, "released",
                   G_CALLBACK(+[](GtkGestureClick* c, int n, double x, double y, Browser* self) {
                     self->mouse_x_ = x;
                     self->mouse_y_ = y;
                     const int button = self->drag_button_;
                     self->drag_button_ = 0;
                     if (!self->browser_ || button > 3)
                       return;
                     self->browser_->GetHost()->SendMouseClickEvent(
                         self->mouse(gtk_event_controller_get_current_event_state(GTK_EVENT_CONTROLLER(c))),
                         button == 2   ? MBT_MIDDLE
                         : button == 3 ? MBT_RIGHT
                                       : MBT_LEFT,
                         true, std::min(n, 3));
                   }),
                   this);
  gtk_widget_add_controller(area_, GTK_EVENT_CONTROLLER(click));
  GtkEventController* scroll = gtk_event_controller_scroll_new(GTK_EVENT_CONTROLLER_SCROLL_BOTH_AXES);
  g_signal_connect(
      scroll, "scroll",
      G_CALLBACK(+[](GtkEventControllerScroll* c, double dx, double dy, Browser* self) -> gboolean {
        if (!self->browser_)
          return false;
        const double factor = gtk_event_controller_scroll_get_unit(c) == GDK_SCROLL_UNIT_SURFACE ? 1.0 : 40.0;
        self->browser_->GetHost()->SendMouseWheelEvent(
            self->mouse(gtk_event_controller_get_current_event_state(GTK_EVENT_CONTROLLER(c))),
            std::lround(-dx * factor), std::lround(-dy * factor));
        return true;
      }),
      this);
  gtk_widget_add_controller(area_, scroll);
  GtkEventController* keys = gtk_event_controller_key_new();
  g_signal_connect(
      keys, "key-pressed",
      G_CALLBACK(+[](GtkEventControllerKey* c, guint key, guint code, GdkModifierType state,
                     Browser* self) -> gboolean {
        if ((state & (GDK_CONTROL_MASK | GDK_SHIFT_MASK | GDK_ALT_MASK | GDK_SUPER_MASK)) ==
                GDK_CONTROL_MASK &&
            gdk_keyval_to_lower(key) == GDK_KEY_t && self->owner()) {
          g_action_group_activate_action(G_ACTION_GROUP(self->owner()->window), "new-tab", nullptr);
          return true;
        }
        if ((state & (GDK_CONTROL_MASK | GDK_SHIFT_MASK | GDK_ALT_MASK | GDK_SUPER_MASK)) ==
                GDK_CONTROL_MASK &&
            (key == GDK_KEY_Page_Down || key == GDK_KEY_Page_Up) && self->owner()) {
          g_action_group_activate_action(G_ACTION_GROUP(self->owner()->window),
                                         key == GDK_KEY_Page_Down ? "next-tab" : "previous-tab", nullptr);
          return true;
        }
        if (((state & GDK_CONTROL_MASK) && gdk_keyval_to_lower(key) == GDK_KEY_v) ||
            ((state & GDK_SHIFT_MASK) && key == GDK_KEY_Insert)) {
          Clipboard::paste(*self, (state & GDK_CONTROL_MASK) && (state & GDK_SHIFT_MASK));
          return true;
        }
        self->send_key(key, code, state, false);
        GdkEvent* event = gtk_event_controller_get_current_event(GTK_EVENT_CONTROLLER(c));
        if (key == GDK_KEY_space && !self->im_preedit_active_)
          self->im_space_event_ = key_event(key, code, state, KEYEVENT_CHAR);
        const bool handled = event && gtk_im_context_filter_keypress(self->im_, event);
        if (!handled && self->browser_ && self->im_space_event_)
          self->browser_->GetHost()->SendKeyEvent(*self->im_space_event_);
        self->im_space_event_.reset();
        if (!handled && self->browser_ && (key == GDK_KEY_Return || key == GDK_KEY_KP_Enter) &&
            !(state & (GDK_CONTROL_MASK | GDK_ALT_MASK | GDK_SUPER_MASK))) {
          // Chromium activates Enter controls on CHAR; IM composition must consume it first.
          auto character = key_event(key, code, state, KEYEVENT_CHAR);
          character.character = character.unmodified_character = '\r';
          self->browser_->GetHost()->SendKeyEvent(character);
        }
        return true;
      }),
      this);
  g_signal_connect(
      keys, "key-released",
      G_CALLBACK(+[](GtkEventControllerKey* c, guint key, guint code, GdkModifierType state, Browser* self) {
        self->send_key(key, code, state, true);
        GdkEvent* event = gtk_event_controller_get_current_event(GTK_EVENT_CONTROLLER(c));
        if (event)
          gtk_im_context_filter_keypress(self->im_, event);
      }),
      this);
  gtk_widget_add_controller(area_, keys);
}
CefMouseEvent Browser::mouse(GdkModifierType state) {
  CefMouseEvent e;
  e.x = std::lround(mouse_x_);
  e.y = std::lround(mouse_y_);
  e.modifiers = modifiers(state);
  return e;
}
void Browser::send_key(guint keyval, guint keycode, GdkModifierType state, bool release) {
  if (!release) {
    clipboard_until_ = g_get_monotonic_time() + 5 * G_TIME_SPAN_SECOND;
    if (!closing && browser_ && clipboard_allowed()) browser_->GetHost()->SetFocus(true);
  }
  if (!browser_)
    return;
  browser_->GetHost()->SendKeyEvent(key_event(keyval, keycode, state, release ? KEYEVENT_KEYUP : KEYEVENT_RAWKEYDOWN));
}
void Browser::OnTitleChange(CefRefPtr<CefBrowser> source, const CefString& title) {
  if (!owns(source))
    return;
  name = Policy::title(url, title);
  changed();
}
namespace {
class FaviconDownload final : public CefDownloadImageCallback {
public:
  FaviconDownload(CefRefPtr<Browser> owner, const std::vector<CefString>& urls)
    : owner_(owner), urls_(urls), page_(owner->url) {}
  void next() {
    if (owner_->closing || !owner_->cef_browser() || index_ >= urls_.size()) return;
    owner_->cef_browser()->GetHost()->DownloadImage(urls_[index_++], true, 64, false, this);
  }
  void OnDownloadImageFinished(const CefString& uri, int status, CefRefPtr<CefImage> image) override {
    if (!owner_->on_favicon || owner_->closing) return;
    int width, height;
    auto png = image && !image->IsEmpty() && (status == 0 || (status >= 200 && status < 300))
      ? image->GetAsPNG(1.0f, true, width, height) : nullptr;
    if (!png || png->GetSize() > 2 * 1024 * 1024) { next(); return; }
    std::vector<unsigned char> buffer(png->GetSize());
    png->GetData(buffer.data(), buffer.size(), 0);
    GBytes* bytes = g_bytes_new(buffer.data(), buffer.size());
    GdkTexture* texture = gdk_texture_new_from_bytes(bytes, nullptr);
    g_bytes_unref(bytes);
    if (!texture) { next(); return; }
    auto callback = owner_->on_favicon;
    if (callback) callback(page_, uri.ToString(), texture);
    g_object_unref(texture);
  }
private:
  CefRefPtr<Browser> owner_;
  std::vector<CefString> urls_;
  std::string page_;
  size_t index_ = 0;
  IMPLEMENT_REFCOUNTING(FaviconDownload);
};
}
void Browser::OnFaviconURLChange(CefRefPtr<CefBrowser> source, const std::vector<CefString>& urls) {
  if (!owns(source) || closing || urls.empty() || !on_favicon) return;
  CefRefPtr<FaviconDownload> download = new FaviconDownload(this, urls);
  download->next();
}
void Browser::OnAddressChange(CefRefPtr<CefBrowser> source, CefRefPtr<CefFrame> frame,
                              const CefString& address) {
  if (!owns(source) || !frame->IsMain() || (address == "about:blank" && url != "about:blank"))
    return;
  bool navigated = url != address.ToString();
  url = address;
  if (navigated && on_favicon) on_favicon(url, "", nullptr);
  changed();
}
void Browser::OnLoadingProgressChange(CefRefPtr<CefBrowser> source, double value) {
  if (owns(source)) {
    progress = value;
    changed();
  }
}
void Browser::OnLoadingStateChange(CefRefPtr<CefBrowser> source, bool value, bool, bool) {
  if (!owns(source))
    return;
  loading = value;
  if (!value) navigation_started_ = false;
  changed();
}
void Browser::OnLoadStart(CefRefPtr<CefBrowser> source, CefRefPtr<CefFrame> frame, TransitionType transition) {
  if (!owns(source) || !frame->IsMain() || closing || (frame->GetURL() == "about:blank" && url != "about:blank")) return;
  if ((transition & TT_FORWARD_BACK_FLAG) && !navigation_started_) {
    // Chromium restores cached documents without calling OnBeforeBrowse.
    page_message_context.clear();
    cancel_authentication();
    certificate_failure_.reset();
    ++main_navigation_epoch_;
    main_navigation_uri_ = frame->GetURL();
  }
  if (!main_navigation_uri_.empty() && frame->GetURL() != main_navigation_uri_) return;
  loading_document_frame_ = frame->GetIdentifier().ToString();
  if (!navigation_started_ && on_load_start) {
    auto callback = on_load_start;
    callback(frame->GetURL());
  }
  navigation_started_ = false;
}
void Browser::OnLoadEnd(CefRefPtr<CefBrowser> source, CefRefPtr<CefFrame> frame, int status) {
  if (!owns(source) || !frame->IsMain())
    return;
  if (frame->GetURL() == "about:blank" && url != "about:blank")
    return;
  // A replaced error document can finish after its replacement has been requested.
  if (loading_document_frame_ != frame->GetIdentifier().ToString()) return;
  loading_document_frame_.clear();
  if (on_load_end)
    on_load_end(status);
}
void Browser::OnLoadError(CefRefPtr<CefBrowser> source, CefRefPtr<CefFrame> frame, ErrorCode error,
                          const CefString& text, const CefString& failed_url) {
  if (!owns(source) || !frame->IsMain() || closing) return;
  const auto failed_uri = failed_url.ToString();
  const bool current_failure = certificate_failure_ && certificate_failure_->navigation == main_navigation_epoch_ &&
    certificate_failure_->uri == main_navigation_uri_ && certificate_failure_->uri == failed_uri;
  if (error == ERR_ABORTED) {
    if (current_failure) certificate_failure_.reset();
    return;
  }
  if (current_failure && certificate_failure_->error == error) {
    auto failure = std::exchange(certificate_failure_, {});
    auto callback = on_tls_error;
    if (callback && callback(failure->uri, failure->info)) return;
  }
  // A later navigation must not be replaced by an older request's error page.
  if (failed_uri != (main_navigation_uri_.empty() ? url : main_navigation_uri_)) return;
  auto callback = on_load_error;
  if (callback && callback(failed_url, text)) return;
  show_error("Unable to Display This Website", text.ToString());
}
void Browser::OnFullscreenModeChange(CefRefPtr<CefBrowser> source, bool fullscreen) {
  if (!owns(source))
    return;
  if (!owner())
    return;
  if (fullscreen)
    gtk_window_fullscreen(owner()->window);
  else
    gtk_window_unfullscreen(owner()->window);
}
bool Browser::OnTooltip(CefRefPtr<CefBrowser>, CefString& text) {
  gtk_widget_set_tooltip_text(area_, text.ToString().c_str());
  return true;
}
bool Browser::OnProcessMessageReceived(CefRefPtr<CefBrowser> source_browser, CefRefPtr<CefFrame> frame,
                                       CefProcessId source, CefRefPtr<CefProcessMessage> message) {
  if (!owns(source_browser))
    return false;
  return PageMessages::receive(*this, frame, source, message) || Clipboard::receive(*this, frame, source, message);
}
bool Browser::OnConsoleMessage(CefRefPtr<CefBrowser>, cef_log_severity_t, const CefString& message,
                               const CefString& source, int) {
  const auto prefix = "chrome-extension://" + Extensions::id_for_path(GNOSIS_BRIDGE_DIR) + "/";
  if (source.ToString().starts_with(prefix) && message.ToString().starts_with("GNOSIS_NATIVE_TAB_ID:")) {
    if (auto id = extension_tab_id(message.ToString()))
      native_tab_id = *id;
    return true;
  }
  if (owner() && owner()->app.render_test)
    g_print("Gnosis page: %s\n", message.ToString().c_str());
  return false;
}
bool Browser::OnCursorChange(CefRefPtr<CefBrowser> source, CefCursorHandle, cef_cursor_type_t type,
                             const CefCursorInfo& info) {
  if (!owns(source))
    return true;
  if (type == CT_CUSTOM) {
    if (GdkCursor* cursor = make_custom_cursor(info)) {
      gtk_widget_set_cursor(area_, cursor);
      g_object_unref(cursor);
      custom_cursor_seen = true;
      return true;
    }
  }
  const char* cursor = "default";
  switch (type) {
  case CT_HAND:
    cursor = "pointer";
    break;
  case CT_IBEAM:
    cursor = "text";
    break;
  case CT_CROSS:
    cursor = "crosshair";
    break;
  case CT_MOVE:
    cursor = "move";
    break;
  case CT_NORTHSOUTHRESIZE:
    cursor = "ns-resize";
    break;
  case CT_EASTWESTRESIZE:
    cursor = "ew-resize";
    break;
  case CT_NORTHEASTSOUTHWESTRESIZE:
    cursor = "nesw-resize";
    break;
  case CT_NORTHWESTSOUTHEASTRESIZE:
    cursor = "nwse-resize";
    break;
  case CT_GRAB:
    cursor = "grab";
    break;
  case CT_GRABBING:
    cursor = "grabbing";
    break;
  case CT_NONE:
    cursor = "none";
    break;
  default:
    break;
  }
  gtk_widget_set_cursor_from_name(area_, cursor);
  return true;
}
bool Browser::OnShowPermissionPrompt(CefRefPtr<CefBrowser> source, uint64_t id, const CefString& origin,
                                     uint32_t permissions, CefRefPtr<CefPermissionPromptCallback> callback) {
  if (!owns(source) || closing || !owner()) {
    callback->Continue(CEF_PERMISSION_RESULT_DENY);
    return true;
  }
  std::string detail;
  const std::pair<uint32_t, const char*> descriptions[] = {
      {CEF_PERMISSION_TYPE_CAMERA_STREAM, "Use your camera"},
      {CEF_PERMISSION_TYPE_MIC_STREAM, "Use your microphone"},
      {CEF_PERMISSION_TYPE_NOTIFICATIONS, "Send notifications"},
      {CEF_PERMISSION_TYPE_GEOLOCATION, "Access your location"},
      {CEF_PERMISSION_TYPE_CLIPBOARD, "Access the clipboard"},
      {CEF_PERMISSION_TYPE_LOCAL_FONTS, "Use local fonts"},
      {CEF_PERMISSION_TYPE_FILE_SYSTEM_ACCESS, "Access files"},
      {CEF_PERMISSION_TYPE_MULTIPLE_DOWNLOADS, "Download multiple files"},
      {CEF_PERMISSION_TYPE_POINTER_LOCK, "Lock the pointer"},
      {CEF_PERMISSION_TYPE_KEYBOARD_LOCK, "Capture keyboard input"},
      {CEF_PERMISSION_TYPE_LOCAL_NETWORK, "Access your local network"},
      {CEF_PERMISSION_TYPE_LOOPBACK_NETWORK, "Connect to local services"},
      {CEF_PERMISSION_TYPE_STORAGE_ACCESS, "Access stored website data"},
      {CEF_PERMISSION_TYPE_TOP_LEVEL_STORAGE_ACCESS, "Access stored website data"},
      {CEF_PERMISSION_TYPE_WINDOW_MANAGEMENT, "Manage windows"},
      {CEF_PERMISSION_TYPE_WEB_APP_INSTALLATION, "Install a web application"},
      {CEF_PERMISSION_TYPE_IDLE_DETECTION, "Detect when you are away"},
      {CEF_PERMISSION_TYPE_MIDI_SYSEX, "Access MIDI devices"},
      {CEF_PERMISSION_TYPE_VR_SESSION, "Use virtual reality"},
      {CEF_PERMISSION_TYPE_AR_SESSION, "Use augmented reality"},
      {CEF_PERMISSION_TYPE_HAND_TRACKING, "Track hand movement"},
      {CEF_PERMISSION_TYPE_CAPTURED_SURFACE_CONTROL, "Control a shared surface"},
      {CEF_PERMISSION_TYPE_CAMERA_PAN_TILT_ZOOM, "Control your camera"},
      {CEF_PERMISSION_TYPE_DISK_QUOTA, "Store additional data"},
      {CEF_PERMISSION_TYPE_IDENTITY_PROVIDER, "Use your identity provider"},
      {CEF_PERMISSION_TYPE_REGISTER_PROTOCOL_HANDLER, "Open links for a protocol"},
      {CEF_PERMISSION_TYPE_PROTECTED_MEDIA_IDENTIFIER, "Use protected media"},
      {CEF_PERMISSION_TYPE_SENSORS, "Read device sensors"},
      {CEF_PERMISSION_TYPE_LOCAL_NETWORK_ACCESS_DEPRECATED, "Access your local network"}};
  uint32_t known = 0;
  for (auto [flag, label] : descriptions) {
    known |= flag;
    if (permissions & flag)
      detail += (detail.empty() ? "" : "\n") + std::string("• ") + label;
  }
  if (!permissions || (permissions & ~known)) {
    callback->Continue(CEF_PERMISSION_RESULT_DENY);
    return true;
  }
  EphyEnginePermissionKind kind = EPHY_ENGINE_PERMISSION_OTHER;
  switch (permissions) {
    case CEF_PERMISSION_TYPE_NOTIFICATIONS: kind = EPHY_ENGINE_PERMISSION_NOTIFICATIONS; break;
    case CEF_PERMISSION_TYPE_GEOLOCATION: kind = EPHY_ENGINE_PERMISSION_LOCATION; break;
    case CEF_PERMISSION_TYPE_CLIPBOARD: kind = EPHY_ENGINE_PERMISSION_CLIPBOARD; break;
    case CEF_PERMISSION_TYPE_STORAGE_ACCESS:
    case CEF_PERMISSION_TYPE_TOP_LEVEL_STORAGE_ACCESS: kind = EPHY_ENGINE_PERMISSION_STORAGE_ACCESS; break;
  }
  auto *request = ephy_engine_permission_new(kind, origin.ToString().c_str(),
    source->GetMainFrame()->GetURL().ToString().c_str(), host->session, detail.c_str(),
    [callback](auto decision) {
      callback->Continue(decision == EPHY_ENGINE_PERMISSION_ALLOW ? CEF_PERMISSION_RESULT_ACCEPT :
                         decision == EPHY_ENGINE_PERMISSION_DENY ? CEF_PERMISSION_RESULT_DENY : CEF_PERMISSION_RESULT_IGNORE);
    });
  if (!request) return true;
  auto owned = std::shared_ptr<EphyEnginePermission>(request, g_object_unref);
  permission_prompts_[id] = owned;
  present_permission(request);
  return true;
}
void Browser::present_permission(EphyEnginePermission *request) {
  if (!on_permission || !on_permission(request))
    ephy_engine_permission_decide(request, EPHY_ENGINE_PERMISSION_DISMISS);
}
void Browser::OnDismissPermissionPrompt(CefRefPtr<CefBrowser> source, uint64_t id, cef_permission_request_result_t) {
  if (!owns(source)) return;
  auto it = permission_prompts_.find(id);
  if (it == permission_prompts_.end()) return;
  auto request = std::move(it->second); permission_prompts_.erase(it);
  if (ephy_engine_permission_get_kind(request.get()) == EPHY_ENGINE_PERMISSION_CLIPBOARD) {
    auto context = source->GetHost()->GetRequestContext();
    std::string origin = ephy_engine_permission_get_origin(request.get());
    std::string top = ephy_engine_permission_get_top_origin(request.get());
    // Chromium records the decision after OnDismiss returns; clipboard choices are per request.
    CefPostTask(TID_UI, new ResponseTask([context, origin, top] {
      if (context) context->SetContentSetting(origin, top, CEF_CONTENT_SETTING_TYPE_CLIPBOARD_READ_WRITE, CEF_CONTENT_SETTING_VALUE_DEFAULT);
    }));
  }
  ephy_engine_permission_cancel(request.get());
}
bool Browser::OnRequestMediaAccessPermission(CefRefPtr<CefBrowser> source, CefRefPtr<CefFrame> frame,
  const CefString& origin, uint32_t permissions, CefRefPtr<CefMediaAccessCallback> callback) {
  const uint32_t devices = CEF_MEDIA_PERMISSION_DEVICE_AUDIO_CAPTURE | CEF_MEDIA_PERMISSION_DEVICE_VIDEO_CAPTURE;
  // Display capture needs a real source chooser; never grant a default screen.
  if (!owns(source) || closing || !owner() || !permissions || (permissions & ~devices)) return false;
  auto kind = permissions == devices ? EPHY_ENGINE_PERMISSION_CAMERA_AND_MICROPHONE :
    permissions == CEF_MEDIA_PERMISSION_DEVICE_AUDIO_CAPTURE ? EPHY_ENGINE_PERMISSION_MICROPHONE : EPHY_ENGINE_PERMISSION_CAMERA;
  auto *request = ephy_engine_permission_new(kind, origin.ToString().c_str(),
    source->GetMainFrame()->GetURL().ToString().c_str(), host->session, nullptr,
    [callback, permissions](auto decision) { callback->Continue(decision == EPHY_ENGINE_PERMISSION_ALLOW ? permissions : 0); });
  if (!request) return true;
  std::erase_if(media_requests_, [](const auto& entry) { return ephy_engine_permission_is_resolved(entry.request.get()); });
  auto owned = std::shared_ptr<EphyEnginePermission>(request, g_object_unref);
  media_requests_.push_back({frame->GetIdentifier(), owned});
  auto state = ephy_engine_permission_get_state(request);
  if (state == EPHY_ENGINE_PERMISSION_STATE_ASK) present_permission(request);
  else ephy_engine_permission_decide(request, state == EPHY_ENGINE_PERMISSION_STATE_ALLOW ? EPHY_ENGINE_PERMISSION_ALLOW : EPHY_ENGINE_PERMISSION_DENY);
  return true;
}
bool Browser::GetAuthCredentials(CefRefPtr<CefBrowser> source, const CefString& origin_url,
                                 bool is_proxy, const CefString& auth_host, int port,
                                 const CefString& realm, const CefString& scheme,
                                 CefRefPtr<CefAuthCallback> callback) {
  // Only immutable CEF metadata and the atomic navigation token are read on IO.
  CefRefPtr<Browser> self = this;
  const auto generation = authentication_generation_.load();
  const auto origin = origin_url.ToString(), hostname = auth_host.ToString();
  const auto auth_realm = realm.ToString(), auth_scheme = scheme.ToString();
  if (!CefPostTask(TID_UI, new ResponseTask([self, source, callback, generation, origin,
                                            hostname, port, auth_realm, auth_scheme, is_proxy] {
    if (!self->owns(source) || self->closing || !self->host || hostname.empty() ||
        port < 1 || port > 65535 || generation != self->authentication_generation_.load() ||
        !gtk_widget_get_mapped(self->area_)) {
      callback->Cancel();
      return;
    }
    auto *session = self->host->session ? self->host->session : ephy_engine_session_get_default();
    auto context = source->GetHost()->GetRequestContext();
    if (!context->IsSame(ephy_engine_session_context(session))) {
      callback->Cancel();
      return;
    }
    const auto id = ++self->authentication_serial_;
    auto *request = ephy_engine_authentication_new(origin.c_str(), hostname.c_str(), port,
      auth_realm.c_str(), auth_scheme.c_str(), is_proxy, session,
      [self, source, context, callback, generation, id](const char *username, const char *password) {
        // Erase before completing CEF: completion may reenter navigation or the dialog.
        self->authentication_requests_.erase(id);
        if (!username || !self->owns(source) || self->closing || !self->host ||
            generation != self->authentication_generation_.load() ||
            !gtk_widget_get_mapped(self->area_) ||
            !context->IsSame(source->GetHost()->GetRequestContext()) ||
            !context->IsSame(ephy_engine_session_context(self->host->session ?
              self->host->session : ephy_engine_session_get_default()))) {
          callback->Cancel();
          return false;
        }
        callback->Continue(username, password);
        return true;
      });
    auto owned = std::shared_ptr<EphyEngineAuthentication>(request, g_object_unref);
    self->authentication_requests_.emplace(id, owned);
    const auto present = self->on_authentication;
    if (!present || !present(request)) ephy_engine_authentication_cancel(request);
  }))) callback->Cancel();
  return true;
}
bool Browser::OnSelectClientCertificate(CefRefPtr<CefBrowser> source, bool is_proxy,
                                         const CefString& certificate_host, int port,
                                         const X509CertificateList& certificates,
                                         CefRefPtr<CefSelectClientCertificateCallback> callback) {
  // Always handle manually: Chromium's fallback may choose a certificate.
  CefRefPtr<Browser> self = this;
  const auto hostname = certificate_host.ToString();
  if (!owns(source) || closing || !host || hostname.empty() || port < 1 || port > 65535 ||
      !gtk_widget_get_mapped(area_)) {
    callback->Select(nullptr);
    return true;
  }
  auto *session = host->session ? host->session : ephy_engine_session_get_default();
  auto context = source->GetHost()->GetRequestContext();
  if (!context || !context->IsSame(ephy_engine_session_context(session))) {
    callback->Select(nullptr);
    return true;
  }
  const auto generation = authentication_generation_.load();
  const auto id = ++authentication_serial_;
  auto *request = ephy_engine_client_certificate_new(hostname.c_str(), port, is_proxy, certificates, session,
    [self, source, context, callback, generation, id](CefRefPtr<CefX509Certificate> certificate) {
      // Unlink first: native completion and UI cancellation may reenter teardown.
      self->client_certificate_requests_.erase(id);
      if (!certificate || !self->owns(source) || self->closing || !self->host ||
          generation != self->authentication_generation_.load() || !gtk_widget_get_mapped(self->area_) ||
          !context->IsSame(source->GetHost()->GetRequestContext()) ||
          !context->IsSame(ephy_engine_session_context(self->host->session ?
            self->host->session : ephy_engine_session_get_default()))) {
        callback->Select(nullptr);
        return false;
      }
      callback->Select(certificate);
      return true;
    });
  if (!request) return true;
  auto owned = std::shared_ptr<EphyEngineClientCertificate>(request, g_object_unref);
  client_certificate_requests_.emplace(id, owned);
  const auto present = on_client_certificate;
  if (!present || !present(request)) ephy_engine_client_certificate_cancel(request);
  return true;
}
void Browser::cancel_authentication(bool invalidate_resources) {
  ++authentication_generation_;
  if (invalidate_resources) {
    std::lock_guard lock(resource_frame_mutex_);
    ++resource_lifetime_;
    resource_frame_generations_.clear();
  }
  auto requests = std::exchange(authentication_requests_, {});
  auto certificates = std::exchange(client_certificate_requests_, {});
  for (const auto& [id, request] : requests) ephy_engine_authentication_cancel(request.get());
  for (const auto& [id, request] : certificates) ephy_engine_client_certificate_cancel(request.get());
}
void Browser::cancel_permissions() {
  auto prompts = std::move(permission_prompts_);
  auto media = std::move(media_requests_);
  for (auto& [id, request] : prompts) ephy_engine_permission_decide(request.get(), EPHY_ENGINE_PERMISSION_DISMISS);
  for (auto& entry : media) ephy_engine_permission_decide(entry.request.get(), EPHY_ENGINE_PERMISSION_DISMISS);
}
void Browser::cancel_media_permissions(const std::string& frame) {
  std::vector<std::shared_ptr<EphyEnginePermission>> cancelled;
  std::erase_if(media_requests_, [&](const auto& entry) {
    if (entry.frame != frame) return false;
    cancelled.push_back(entry.request); return true;
  });
  for (auto& request : cancelled) ephy_engine_permission_decide(request.get(), EPHY_ENGINE_PERMISSION_DISMISS);
}
void Browser::OnFrameDetached(CefRefPtr<CefBrowser> source, CefRefPtr<CefFrame> frame) {
  if (!owns(source) || !frame) return;
  {
    std::lock_guard lock(resource_frame_mutex_);
    ++resource_frame_generations_[frame->GetIdentifier().ToString()];
  }
  if (!media_requests_.empty()) cancel_media_permissions(frame->GetIdentifier());
}
bool Browser::route(const std::string& target, bool user_gesture, bool new_tab,
                    CefRefPtr<CefFrame> source_frame, bool initiator_known) {
  if (!owner())
    return false;
  if (Policy::navigable(target)) {
    if (new_tab) {
      owner()->open(target);
      return true;
    }
    return false;
  }
  if (user_gesture && source_frame && source_frame->IsMain() && on_external_uri) {
    // GetURL is the committed frame URL; the pending target is not its origin.
    const auto context = source_frame->GetURL().ToString();
    if (!context.empty()) on_external_uri(target, context, initiator_known);
  }
  return true;
}
bool Browser::OnBeforeBrowse(CefRefPtr<CefBrowser> source, CefRefPtr<CefFrame> frame, CefRefPtr<CefRequest> request,
                             bool user_gesture, bool is_redirect) {
  if (!owns(source) || !frame) return false;
  auto target = request->GetURL().ToString();
  // CEF identifies the destination here, which may have a different initiator.
  if (frame->IsMain() && route(target, user_gesture, false, frame, false)) return true;
  if (!is_redirect && !frame->IsMain()) {
    std::lock_guard lock(resource_frame_mutex_);
    ++resource_frame_generations_[frame->GetIdentifier().ToString()];
  }
  if (frame->IsMain()) {
    page_message_context.clear();
    cancel_permissions();
    cancel_authentication(!is_redirect);
    certificate_failure_.reset();
    ++main_navigation_epoch_;
    main_navigation_uri_ = target;
    loading_document_frame_.clear();
    if (!host || !host->session || !ephy_engine_session_is_ephemeral(host->session))
      ephy_engine_certificate_reset_cached_decisions(source->GetHost()->GetRequestContext(), request->GetURL(), false);
  }
  else if (!media_requests_.empty()) cancel_media_permissions(frame->GetIdentifier());
  // Overall loading also changes for fragments and subframes, which keep the document.
  if (frame->IsMain() && !is_redirect && on_load_start &&
      (target != "about:blank" || url == "about:blank")) {
    navigation_started_ = true;
    auto callback = on_load_start;
    callback(target);
  }
  return false;
}
CefRefPtr<CefResourceRequestHandler> Browser::GetResourceRequestHandler(CefRefPtr<CefBrowser>,
    CefRefPtr<CefFrame> frame, CefRefPtr<CefRequest> request, bool, bool, const CefString&, bool&) {
  if (frame && frame->IsMain() && request->GetResourceType() == RT_MAIN_FRAME)
    main_request_.store(request->GetIdentifier());
  return this;
}
cef_return_value_t Browser::OnBeforeResourceLoad(CefRefPtr<CefBrowser> source, CefRefPtr<CefFrame> frame,
    CefRefPtr<CefRequest> request, CefRefPtr<CefCallback> callback) {
  // Redirects may revisit this callback; retain the originating document even
  // if an old keepalive request redirects after its frame has navigated away.
  if (!resource_generations_.contains(request->GetIdentifier())) {
    std::lock_guard lock(resource_frame_mutex_);
    const auto frame_id = frame ? frame->GetIdentifier().ToString() : std::string();
    resource_generations_.emplace(request->GetIdentifier(), ResourceGeneration {
      resource_lifetime_, frame_id, resource_frame_generations_[frame_id]});
  }
  auto agent = request_user_agent_.load();
  ephy_engine_settings_apply_request(request, agent.get());
  if (session_request_handler_) return session_request_handler_->OnBeforeResourceLoad(source, frame, request, callback);
  return RV_CONTINUE;
}
void Browser::OnResourceLoadComplete(CefRefPtr<CefBrowser> source, CefRefPtr<CefFrame> frame,
    CefRefPtr<CefRequest> request, CefRefPtr<CefResponse> response, URLRequestStatus status, int64_t) {
  auto found = resource_generations_.find(request->GetIdentifier());
  if (found == resource_generations_.end()) return;
  const auto generation = found->second;
  resource_generations_.erase(found);
  if (status != UR_SUCCESS || !frame || !response) return;
  CefRefPtr<Browser> self = this;
  auto uri = request->GetURL().ToString();
  CefPostTask(TID_UI, new ResponseTask([self, source, frame, response, uri, generation] {
    if (!self->owns(source) || self->closing || !frame->IsValid() ||
        frame->GetIdentifier().ToString() != generation.frame) return;
    {
      std::lock_guard lock(self->resource_frame_mutex_);
      if (self->resource_lifetime_ != generation.lifetime ||
          self->resource_frame_generations_[generation.frame] != generation.document) return;
    }
    auto callback = self->on_resource_complete;
    if (callback) callback(uri, response);
  }));
}
bool Browser::OnResourceResponse(CefRefPtr<CefBrowser> source, CefRefPtr<CefFrame> frame,
    CefRefPtr<CefRequest> request, CefRefPtr<CefResponse> response) {
  if (!frame || !frame->IsMain() || request->GetResourceType() != RT_MAIN_FRAME) return false;
  // CEF delivers network callbacks on IO; GTK and view callbacks run on UI.
  CefRefPtr<Browser> self = this;
  auto uri = request->GetURL().ToString();
  auto id = request->GetIdentifier();
  CefPostTask(TID_UI, new ResponseTask([self, source, response, uri, id] {
    if (self->owns(source) && !self->closing && self->main_request_.load() == id && self->on_response)
      self->on_response(uri, response);
  }));
  return false;
}
bool Browser::OnOpenURLFromTab(CefRefPtr<CefBrowser> source, CefRefPtr<CefFrame> frame, const CefString& target,
                               cef_window_open_disposition_t, bool user_gesture) {
  // CEF may substitute the main frame when it cannot resolve the actual source.
  return !owns(source) || route(target, user_gesture, true, frame, false);
}
bool Browser::OnBeforePopup(CefRefPtr<CefBrowser> source, CefRefPtr<CefFrame> frame, int popup_id, const CefString& target,
                            const CefString&, cef_window_open_disposition_t, bool user_gesture,
                            const CefPopupFeatures& features, CefWindowInfo& info, CefRefPtr<CefClient>& client,
                            CefBrowserSettings&, CefRefPtr<CefDictionaryValue>&, bool*) {
  if (!owns(source) || closing || !owner() || pending_popups_.contains(popup_id)) return true;
  if (!target.empty() && !Policy::navigable(target))
    return route(target, user_gesture, true, frame, true);
  CefRefPtr<Browser> keep_alive = this;
  const auto context = source->GetHost()->GetRequestContext();
  const auto address = target.empty() ? std::string("about:blank") : target.ToString();
  auto create = on_create_popup;
  auto popup = create ? create(address, features) : owner()->open(address, true, true);
  if (closing || !owns(source) || !popup || popup.get() == this || popup->closing ||
      popup->loaded || popup->creating_ || popup->waiting_context_ || popup->browser_ ||
      popup->browser_id_ || popup->popup_opener_ || !popup->host || popup->host->closing ||
      popup->host->destroyed) return true;
  auto popup_context = ephy_engine_session_context(
      popup->host->session ? popup->host->session : ephy_engine_session_get_default());
  if (!context || !popup_context || !context->IsSame(popup_context)) return true;
  // Mapping the GTK child must never start a second, unrelated CEF browser.
  popup->defer_start = true;
  popup->loaded = true;
  popup->creating_ = true;
  ++active_count;
  popup->native_popup_child_ = true;
  popup->popup_opener_ = this;
  popup->popup_id_ = popup_id;
  pending_popups_.emplace(popup_id, popup);
  client = popup;
  info.SetAsWindowless(0);
  info.shared_texture_enabled = !popup->software_;
  return false;
}
bool Browser::OnContentsBoundsChange(CefRefPtr<CefBrowser> source, const CefRect& bounds) {
  if (!owns(source) || closing || !native_popup_child_ || is_extension_popup ||
      !on_contents_bounds_change) return false;
  auto callback = on_contents_bounds_change;
  callback(bounds);
  return true;
}
bool Browser::OnBeforeDownload(CefRefPtr<CefBrowser> source, CefRefPtr<CefDownloadItem> item, const CefString& suggested,
                               CefRefPtr<CefBeforeDownloadCallback> callback) {
  if (!owns(source)) return false;
  downloads_.insert(item->GetId());
  auto *session = host && host->session ? host->session : ephy_engine_session_get_default();
  ephy_engine_download_before(session, source, item, suggested, callback);
  return true;
}
void Browser::OnDownloadUpdated(CefRefPtr<CefBrowser> source, CefRefPtr<CefDownloadItem> item,
                                CefRefPtr<CefDownloadItemCallback> callback) {
  if (!owns(source)) return;
  if (item->IsInProgress()) downloads_.insert(item->GetId());
  else downloads_.erase(item->GetId());
  auto *session = host && host->session ? host->session : ephy_engine_session_get_default();
  ephy_engine_download_updated(session, source, item, callback);
  // Closing a tab must not detach CEF's download handler from an active transfer.
  if (closing && downloads_.empty()) source->GetHost()->CloseBrowser(false);
}
void Browser::OnBeforeContextMenu(CefRefPtr<CefBrowser>, CefRefPtr<CefFrame>, CefRefPtr<CefContextMenuParams>,
                                  CefRefPtr<CefMenuModel>) {}
bool Browser::RunContextMenu(CefRefPtr<CefBrowser>, CefRefPtr<CefFrame>,
                             CefRefPtr<CefContextMenuParams> params, CefRefPtr<CefMenuModel> model,
                             CefRefPtr<CefRunContextMenuCallback> callback) {
  auto* popover = gtk_popover_menu_new_from_model(nullptr);
  gtk_widget_set_parent(popover, area_);
  auto* cb = new CefRefPtr<CefRunContextMenuCallback>(callback);
  g_object_set_data_full(
      G_OBJECT(popover), "cef-callback", cb,
      +[](gpointer p) { delete static_cast<CefRefPtr<CefRunContextMenuCallback>*>(p); });
  auto* actions = g_simple_action_group_new();
  int next = 0;
  std::function<GMenu*(CefRefPtr<CefMenuModel>)> build = [&](auto source) {
    auto* menu = g_menu_new();
    auto* section = g_menu_new();
    for (size_t i = 0; i < source->GetCount(); i++) {
      auto type = source->GetTypeAt(i);
      if (type == MENUITEMTYPE_SEPARATOR) {
        g_menu_append_section(menu, nullptr, G_MENU_MODEL(section));
        g_object_unref(section);
        section = g_menu_new();
        continue;
      }
      auto label = source->GetLabelAt(i).ToString();
      label.erase(std::remove(label.begin(), label.end(), '&'), label.end());
      if (auto submenu = source->GetSubMenuAt(i)) {
        auto* child = build(submenu);
        g_menu_append_submenu(section, label.c_str(), G_MENU_MODEL(child));
        g_object_unref(child);
        continue;
      }
      auto name = "item" + std::to_string(next++);
      auto* action = (type == MENUITEMTYPE_CHECK || type == MENUITEMTYPE_RADIO)
                         ? g_simple_action_new_stateful(name.c_str(), nullptr,
                                                        g_variant_new_boolean(source->IsCheckedAt(i)))
                         : g_simple_action_new(name.c_str(), nullptr);
      g_simple_action_set_enabled(action, source->IsEnabledAt(i));
      g_object_set_data(G_OBJECT(action), "command", GINT_TO_POINTER(source->GetCommandIdAt(i)));
      g_signal_connect(action, "activate", G_CALLBACK(+[](GSimpleAction* action, GVariant*, GtkPopover* pop) {
                         auto* cb = static_cast<CefRefPtr<CefRunContextMenuCallback>*>(
                             g_object_get_data(G_OBJECT(pop), "cef-callback"));
                         if (*cb) {
                           (*cb)->Continue(GPOINTER_TO_INT(g_object_get_data(G_OBJECT(action), "command")),
                                           EVENTFLAG_NONE);
                           *cb = nullptr;
                         }
                         gtk_popover_popdown(pop);
                       }),
                       popover);
      g_action_map_add_action(G_ACTION_MAP(actions), G_ACTION(action));
      g_object_unref(action);
      g_menu_append(section, label.c_str(), ("context." + name).c_str());
    }
    g_menu_append_section(menu, nullptr, G_MENU_MODEL(section));
    g_object_unref(section);
    return menu;
  };
  auto* menu = build(model);
  gtk_widget_insert_action_group(popover, "context", G_ACTION_GROUP(actions));
  g_object_unref(actions);
  gtk_popover_menu_set_menu_model(GTK_POPOVER_MENU(popover), G_MENU_MODEL(menu));
  g_object_unref(menu);
  g_signal_connect(popover, "closed", G_CALLBACK(+[](GtkPopover* pop, gpointer) {
                     auto* cb = static_cast<CefRefPtr<CefRunContextMenuCallback>*>(
                         g_object_get_data(G_OBJECT(pop), "cef-callback"));
                     if (*cb)
                       (*cb)->Cancel();
                     gtk_widget_unparent(GTK_WIDGET(pop));
                   }),
                   nullptr);
  GdkRectangle rect = {params->GetXCoord(), params->GetYCoord(), 1, 1};
  gtk_popover_set_pointing_to(GTK_POPOVER(popover), &rect);
  gtk_popover_popup(GTK_POPOVER(popover));
  return true;
}

namespace {
struct DialogData {
  CefRefPtr<CefJSDialogCallback> callback;
  CefRefPtr<Browser> browser;
  GtkWidget* entry = nullptr;
  bool unload = false;
};
void dialog_response(AdwAlertDialog*, const char* response, DialogData* data) {
  const bool accept = std::strcmp(response, "accept") == 0;
  const char* value = data->entry ? gtk_editable_get_text(GTK_EDITABLE(data->entry)) : "";
  data->callback->Continue(accept, value);
  if (!accept && data->unload) {
    data->browser->closing = false;
    if (data->browser->on_close_cancelled)
      data->browser->on_close_cancelled();
    if (data->browser->owner())
      data->browser->owner()->closing = false;
  }
}
} // namespace
bool Browser::OnJSDialog(CefRefPtr<CefBrowser>, const CefString&, JSDialogType type, const CefString& message,
                         const CefString& prompt, CefRefPtr<CefJSDialogCallback> callback, bool& suppress) {
  if (!owner()) {
    callback->Continue(false, "");
    return true;
  }
  AdwDialog* dialog = adw_alert_dialog_new("Website Message", message.ToString().c_str());
  auto* data = new DialogData{callback, this};
  if (type != JSDIALOGTYPE_ALERT)
    adw_alert_dialog_add_response(ADW_ALERT_DIALOG(dialog), "cancel", "_Cancel");
  adw_alert_dialog_add_response(ADW_ALERT_DIALOG(dialog), "accept", "_OK");
  adw_alert_dialog_set_default_response(ADW_ALERT_DIALOG(dialog), "accept");
  adw_alert_dialog_set_close_response(ADW_ALERT_DIALOG(dialog),
                                      type == JSDIALOGTYPE_ALERT ? "accept" : "cancel");
  if (type == JSDIALOGTYPE_PROMPT) {
    data->entry = gtk_entry_new();
    gtk_editable_set_text(GTK_EDITABLE(data->entry), prompt.ToString().c_str());
    adw_alert_dialog_set_extra_child(ADW_ALERT_DIALOG(dialog), data->entry);
  }
  g_signal_connect_data(
      dialog, "response", G_CALLBACK(dialog_response), data,
      +[](gpointer p, GClosure*) { delete static_cast<DialogData*>(p); }, G_CONNECT_DEFAULT);
  adw_dialog_present(dialog, GTK_WIDGET(owner()->window));
  return true;
}
bool Browser::OnBeforeUnloadDialog(CefRefPtr<CefBrowser>, const CefString&, bool,
                                   CefRefPtr<CefJSDialogCallback> callback) {
  if (!owner()) {
    callback->Continue(true, "");
    return true;
  }
  AdwDialog* dialog = adw_alert_dialog_new("Leave This Page?", "Changes you made may not be saved.");
  adw_alert_dialog_add_response(ADW_ALERT_DIALOG(dialog), "cancel", "_Keep Open");
  adw_alert_dialog_add_response(ADW_ALERT_DIALOG(dialog), "accept", "_Leave");
  adw_alert_dialog_set_response_appearance(ADW_ALERT_DIALOG(dialog), "accept", ADW_RESPONSE_DESTRUCTIVE);
  adw_alert_dialog_set_default_response(ADW_ALERT_DIALOG(dialog), "cancel");
  adw_alert_dialog_set_close_response(ADW_ALERT_DIALOG(dialog), "cancel");
  auto* data = new DialogData{callback, this, nullptr, true};
  g_signal_connect_data(
      dialog, "response", G_CALLBACK(dialog_response), data,
      +[](gpointer p, GClosure*) { delete static_cast<DialogData*>(p); }, G_CONNECT_DEFAULT);
  adw_dialog_present(dialog, GTK_WIDGET(owner()->window));
  return true;
}
