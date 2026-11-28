/* SPDX-License-Identifier: GPL-3.0-or-later */

#include "config.h"
#include "gnosis-shared-ui.hpp"

#include <adwaita.h>
#include <glib/gi18n.h>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <unordered_map>
#include <utility>

#include "ephy-engine-session.h"
#include "ephy-engine-view.h"
#include "ephy-window.h"

namespace GnosisShared {

namespace {

constexpr const char *kDefaultServer = "wss://git.neg-zero.com/gnosis-session";

bool
has_text(GtkEditable *editable)
{
  const char *text = gtk_editable_get_text(editable);
  return text && text[strspn(text, " \t\r\n")] != '\0';
}

void
set_icon_button_accessibility(GtkWidget *button, const char *label)
{
  gtk_widget_set_tooltip_text(button, label);
  gtk_accessible_update_property(GTK_ACCESSIBLE(button),
                                 GTK_ACCESSIBLE_PROPERTY_LABEL, label,
                                 -1);
}

GtkWidget *
new_icon_button(const char *icon, const char *label)
{
  GtkWidget *button = gtk_button_new_from_icon_name(icon);
  gtk_widget_add_css_class(button, "flat");
  set_icon_button_accessibility(button, label);
  return button;
}

}  // namespace

class Interface::Impl {
 public:
  Impl(Interface *owner, GtkApplication *application)
      : owner_(owner), application_(application)
  {
    g_weak_ref_init(&host_window_, nullptr);
    build_dialog();
  }

  ~Impl()
  {
    clearing_ = true;
    detach_host_banner();

    if (guest_window_)
      gtk_window_destroy(GTK_WINDOW(guest_window_));
    g_clear_object(&guest_session_);
    if (dialog_)
      adw_dialog_force_close(ADW_DIALOG(dialog_));

    g_weak_ref_clear(&host_window_);
  }

  void present(GtkWindow *parent)
  {
    if (!dialog_)
      build_dialog();

    if (session_active_)
      adw_view_stack_set_visible_child_name(dialog_stack_, "session");
    else
      adw_view_stack_set_visible_child_name(dialog_stack_, "setup");

    adw_dialog_present(ADW_DIALOG(dialog_), GTK_WIDGET(parent));
  }

  void active(bool host,
              const std::string &title,
              const std::string &invitation)
  {
    session_active_ = true;
    host_ = host;
    invitation_ = invitation;
    adw_view_stack_set_visible_child_name(dialog_stack_, "session");

    adw_preferences_row_set_title(ADW_PREFERENCES_ROW(session_row_),
                                  title.empty() ? _("Shared Session") : title.c_str());
    adw_action_row_set_subtitle(ADW_ACTION_ROW(session_row_),
                                host ? _("Others can see this page as you browse")
                                     : _("Viewing a page shared with you"));
    gtk_editable_set_text(GTK_EDITABLE(invitation_row_), invitation.c_str());
    gtk_widget_set_visible(invitation_row_, host && !invitation.empty());
    gtk_widget_set_sensitive(copy_button_, !invitation.empty());
    adw_preferences_group_set_title(participants_group_,
                                    host ? _("People") : _("In This Session"));
    gtk_widget_set_visible(GTK_WIDGET(participants_group_), host || !participants_.empty());

    set_busy(false);
    ensure_host_banner();
    update_guest_banner();
  }

  void status(const std::string &text)
  {
    if (session_active_) {
      adw_action_row_set_subtitle(ADW_ACTION_ROW(session_row_),
                                  text.empty() ? nullptr : text.c_str());
      return;
    }
    const bool visible = !text.empty();
    adw_preferences_row_set_title(ADW_PREFERENCES_ROW(status_row_),
                                  visible ? text.c_str() : _("Connecting…"));
    gtk_widget_set_visible(status_row_, visible);
    gtk_widget_set_visible(status_spinner_, visible && !session_active_);
    /* A status without an active session is actionable feedback (for example,
     * an invalid invitation), so the user must be able to correct and retry. */
    set_busy(false);
  }

  void attach_host(EphyWindow *window)
  {
    detach_host_banner();
    g_weak_ref_set(&host_window_, window ? G_OBJECT(window) : nullptr);
    ensure_host_banner();
  }

  EphyEngineView *create_guest()
  {
    if (guest_view_)
      return guest_view_;

    guest_window_ = ADW_APPLICATION_WINDOW(adw_application_window_new(application_));
    g_object_add_weak_pointer(G_OBJECT(guest_window_),
                              reinterpret_cast<gpointer *>(&guest_window_));
    gtk_window_set_title(GTK_WINDOW(guest_window_), _("Shared Session"));
    gtk_window_set_default_size(GTK_WINDOW(guest_window_), 960, 720);

    auto *toolbar = ADW_TOOLBAR_VIEW(adw_toolbar_view_new());
    auto *header = ADW_HEADER_BAR(adw_header_bar_new());
    auto *title = adw_window_title_new(_("Shared Session"), _("Live shared browsing"));
    adw_header_bar_set_title_widget(header, title);
    adw_toolbar_view_add_top_bar(toolbar, GTK_WIDGET(header));

    guest_banner_ = adw_banner_new(_("You’re viewing a shared page"));
    adw_banner_set_button_label(ADW_BANNER(guest_banner_), _("Stop Viewing"));
    adw_banner_set_revealed(ADW_BANNER(guest_banner_), TRUE);
    g_signal_connect_swapped(guest_banner_, "button-clicked",
                             G_CALLBACK(stop_clicked), this);
    adw_toolbar_view_add_top_bar(toolbar, guest_banner_);

    GtkWidget *zoom_out = new_icon_button("zoom-out-symbolic", _("Zoom Out"));
    GtkWidget *zoom_fit = new_icon_button("zoom-fit-best-symbolic", _("Fit to Window"));
    GtkWidget *zoom_in = new_icon_button("zoom-in-symbolic", _("Zoom In"));
    GtkWidget *request_size = new_icon_button("video-display-symbolic",
                                               _("Request This Window Size"));
    adw_header_bar_pack_end(header, request_size);
    adw_header_bar_pack_end(header, zoom_in);
    adw_header_bar_pack_end(header, zoom_fit);
    adw_header_bar_pack_end(header, zoom_out);

    g_signal_connect_swapped(zoom_out, "clicked", G_CALLBACK(zoom_out_clicked), this);
    g_signal_connect_swapped(zoom_fit, "clicked", G_CALLBACK(zoom_fit_clicked), this);
    g_signal_connect_swapped(zoom_in, "clicked", G_CALLBACK(zoom_in_clicked), this);
    g_signal_connect_swapped(request_size, "clicked", G_CALLBACK(request_size_clicked), this);

    guest_session_ = ephy_engine_session_new(TRUE);
    guest_view_ = EPHY_ENGINE_VIEW(g_object_new(EPHY_TYPE_ENGINE_VIEW,
                                                "network-session", guest_session_,
                                                nullptr));
    g_object_add_weak_pointer(G_OBJECT(guest_view_),
                              reinterpret_cast<gpointer *>(&guest_view_));
    gtk_widget_set_hexpand(GTK_WIDGET(guest_view_), TRUE);
    gtk_widget_set_vexpand(GTK_WIDGET(guest_view_), TRUE);
    adw_toolbar_view_set_content(toolbar, GTK_WIDGET(guest_view_));
    adw_application_window_set_content(guest_window_, GTK_WIDGET(toolbar));

    g_signal_connect(guest_window_, "close-request",
                     G_CALLBACK(guest_close_requested), this);
    gtk_window_present(GTK_WINDOW(guest_window_));
    update_guest_banner();
    return guest_view_;
  }

  void participant(const std::string &id,
                   const std::string &name,
                   bool pending,
                   bool control)
  {
    remove(id);

    GtkWidget *row;
    if (pending) {
      row = adw_action_row_new();
      adw_preferences_row_set_title(ADW_PREFERENCES_ROW(row),
                                    name.empty() ? _("Guest") : name.c_str());
      adw_action_row_set_subtitle(ADW_ACTION_ROW(row), _("Wants to join"));

      GtkWidget *reject = new_icon_button("window-close-symbolic", _("Decline"));
      GtkWidget *approve = gtk_button_new_with_mnemonic(_("_Allow"));
      gtk_widget_add_css_class(approve, "suggested-action");
      gtk_widget_set_valign(reject, GTK_ALIGN_CENTER);
      gtk_widget_set_valign(approve, GTK_ALIGN_CENTER);
      adw_action_row_add_suffix(ADW_ACTION_ROW(row), reject);
      adw_action_row_add_suffix(ADW_ACTION_ROW(row), approve);
      g_object_set_data_full(G_OBJECT(reject), "participant-id", g_strdup(id.c_str()), g_free);
      g_object_set_data_full(G_OBJECT(approve), "participant-id", g_strdup(id.c_str()), g_free);
      g_signal_connect(approve, "clicked", G_CALLBACK(approve_clicked), this);
      g_signal_connect(reject, "clicked", G_CALLBACK(reject_clicked), this);
    } else {
      row = adw_switch_row_new();
      adw_preferences_row_set_title(ADW_PREFERENCES_ROW(row),
                                    name.empty() ? _("Guest") : name.c_str());
      adw_action_row_set_subtitle(ADW_ACTION_ROW(row), _("Allow control of the page"));
      adw_switch_row_set_active(ADW_SWITCH_ROW(row), control);
      g_object_set_data_full(G_OBJECT(row), "participant-id", g_strdup(id.c_str()), g_free);

      GtkWidget *remove_button = new_icon_button("user-trash-symbolic", _("Remove from Session"));
      gtk_widget_set_valign(remove_button, GTK_ALIGN_CENTER);
      g_object_set_data_full(G_OBJECT(remove_button), "participant-id", g_strdup(id.c_str()), g_free);
      adw_action_row_add_suffix(ADW_ACTION_ROW(row), remove_button);
      g_signal_connect(row, "notify::active", G_CALLBACK(control_toggled), this);
      g_signal_connect(remove_button, "clicked", G_CALLBACK(remove_clicked), this);
    }

    participants_.emplace(id, row);
    adw_preferences_group_add(participants_group_, row);
    gtk_widget_set_visible(GTK_WIDGET(participants_group_), TRUE);
  }

  void remove(const std::string &id)
  {
    auto item = participants_.find(id);
    if (item == participants_.end())
      return;

    adw_preferences_group_remove(participants_group_, item->second);
    participants_.erase(item);
    gtk_widget_set_visible(GTK_WIDGET(participants_group_),
                           host_ || !participants_.empty());
  }

  void ask_resize(const std::string &name,
                  int width,
                  int height,
                  std::function<void(bool)> reply)
  {
    g_autofree char *body = g_strdup_printf(
        _("%s wants the shared page to use %d × %d logical pixels."),
        name.empty() ? _("A guest") : name.c_str(), width, height);
    AdwAlertDialog *dialog = ADW_ALERT_DIALOG(adw_alert_dialog_new(
        _("Change Shared Page Size?"), body));
    adw_alert_dialog_add_response(dialog, "deny", _("_Keep Current Size"));
    adw_alert_dialog_add_response(dialog, "approve", _("_Change Size"));
    adw_alert_dialog_set_default_response(dialog, "approve");
    adw_alert_dialog_set_response_appearance(dialog, "approve", ADW_RESPONSE_SUGGESTED);

    auto *response = new std::function<void(bool)>(std::move(reply));
    g_signal_connect_data(
        dialog, "response",
        G_CALLBACK(+[](AdwAlertDialog *, const char *id, gpointer data) {
          auto *callback = static_cast<std::function<void(bool)> *>(data);
          if (*callback)
            (*callback)(g_strcmp0(id, "approve") == 0);
        }),
        response,
        +[](gpointer data, GClosure *) {
          delete static_cast<std::function<void(bool)> *>(data);
        },
        GConnectFlags(0));

    g_autoptr(GObject) host = G_OBJECT(g_weak_ref_get(&host_window_));
    GtkWidget *parent = host ? GTK_WIDGET(host) : GTK_WIDGET(gtk_application_get_active_window(application_));
    if (parent)
      adw_dialog_present(ADW_DIALOG(dialog), parent);
  }

  void clear()
  {
    clearing_ = true;
    session_active_ = false;
    host_ = false;
    invitation_.clear();
    local_zoom_ = 1.0;

    detach_host_banner();
    for (auto &[id, row] : participants_)
      adw_preferences_group_remove(participants_group_, row);
    participants_.clear();

    if (guest_window_)
      gtk_window_destroy(GTK_WINDOW(guest_window_));
    guest_window_ = nullptr;
    guest_view_ = nullptr;
    guest_banner_ = nullptr;
    g_clear_object(&guest_session_);

    gtk_editable_set_text(GTK_EDITABLE(invitation_row_), "");
    gtk_widget_set_visible(invitation_row_, FALSE);
    gtk_widget_set_visible(status_row_, FALSE);
    gtk_widget_set_visible(GTK_WIDGET(participants_group_), FALSE);
    adw_view_stack_set_visible_child_name(dialog_stack_, "setup");
    set_busy(false);
    clearing_ = false;
  }

 private:
  static void stop_clicked(Impl *self)
  {
    if (self->owner_->stop_requested)
      self->owner_->stop_requested();
  }

  static gboolean guest_close_requested(GtkWindow *, Impl *self)
  {
    if (!self->clearing_ && self->owner_->stop_requested) {
      gtk_widget_set_visible(GTK_WIDGET(self->guest_window_), FALSE);
      self->owner_->stop_requested();
      return TRUE;
    }
    return FALSE;
  }

  static void setup_changed(GtkEditable *, Impl *self)
  {
    self->update_setup_actions();
  }

  static void host_clicked(GtkButton *, Impl *self)
  {
    if (!has_text(GTK_EDITABLE(self->host_name_))) {
      gtk_widget_add_css_class(self->host_name_, "error");
      gtk_widget_grab_focus(self->host_name_);
      return;
    }

    gtk_widget_remove_css_class(self->host_name_, "error");
    self->status(_("Starting shared session…"));
    self->set_busy(true);
    if (self->owner_->host_requested)
      self->owner_->host_requested(
          gtk_editable_get_text(GTK_EDITABLE(self->host_name_)),
          gtk_editable_get_text(GTK_EDITABLE(self->server_entry_)));
  }

  static void join_clicked(GtkButton *, Impl *self)
  {
    const bool valid_invitation = has_text(GTK_EDITABLE(self->join_invitation_));
    const bool valid_name = has_text(GTK_EDITABLE(self->join_name_));
    if (!valid_invitation)
      gtk_widget_add_css_class(self->join_invitation_, "error");
    if (!valid_name)
      gtk_widget_add_css_class(self->join_name_, "error");
    if (!valid_invitation || !valid_name) {
      gtk_widget_grab_focus(valid_invitation ? self->join_name_ : self->join_invitation_);
      return;
    }

    gtk_widget_remove_css_class(self->join_invitation_, "error");
    gtk_widget_remove_css_class(self->join_name_, "error");
    self->status(_("Joining shared session…"));
    self->set_busy(true);
    if (self->owner_->join_requested)
      self->owner_->join_requested(
          gtk_editable_get_text(GTK_EDITABLE(self->join_invitation_)),
          gtk_editable_get_text(GTK_EDITABLE(self->join_name_)));
  }

  static void copy_clicked(GtkButton *, Impl *self)
  {
    if (self->invitation_.empty())
      return;
    GdkClipboard *clipboard = gtk_widget_get_clipboard(self->copy_button_);
    gdk_clipboard_set_text(clipboard, self->invitation_.c_str());
    self->status(_("Invitation copied"));
  }

  static void approve_clicked(GtkButton *button, Impl *self)
  {
    const char *id = static_cast<const char *>(g_object_get_data(G_OBJECT(button), "participant-id"));
    if (id && self->owner_->approved)
      self->owner_->approved(id);
  }

  static void reject_clicked(GtkButton *button, Impl *self)
  {
    const char *id = static_cast<const char *>(g_object_get_data(G_OBJECT(button), "participant-id"));
    if (id && self->owner_->rejected)
      self->owner_->rejected(id);
  }

  static void remove_clicked(GtkButton *button, Impl *self)
  {
    const char *id = static_cast<const char *>(g_object_get_data(G_OBJECT(button), "participant-id"));
    if (id && self->owner_->removed)
      self->owner_->removed(id);
  }

  static void control_toggled(AdwSwitchRow *row, GParamSpec *, Impl *self)
  {
    const char *id = static_cast<const char *>(g_object_get_data(G_OBJECT(row), "participant-id"));
    if (id && self->owner_->control_changed)
      self->owner_->control_changed(id, adw_switch_row_get_active(row));
  }

  static void zoom_out_clicked(Impl *self)
  {
    self->local_zoom_ = std::max(0.5, self->local_zoom_ - 0.1);
    if (self->owner_->zoom_changed)
      self->owner_->zoom_changed(self->local_zoom_);
  }

  static void zoom_fit_clicked(Impl *self)
  {
    self->local_zoom_ = 1.0;
    if (self->owner_->zoom_changed)
      self->owner_->zoom_changed(0.0);
  }

  static void zoom_in_clicked(Impl *self)
  {
    self->local_zoom_ = std::min(3.0, self->local_zoom_ + 0.1);
    if (self->owner_->zoom_changed)
      self->owner_->zoom_changed(self->local_zoom_);
  }

  static void request_size_clicked(Impl *self)
  {
    if (!self->guest_view_ || !self->owner_->resize_requested)
      return;
    self->owner_->resize_requested(gtk_widget_get_width(GTK_WIDGET(self->guest_view_)),
                                   gtk_widget_get_height(GTK_WIDGET(self->guest_view_)));
  }

  void build_dialog()
  {
    dialog_ = GTK_WIDGET(adw_dialog_new());
    g_object_add_weak_pointer(G_OBJECT(dialog_), reinterpret_cast<gpointer *>(&dialog_));
    adw_dialog_set_title(ADW_DIALOG(dialog_), _("Shared Sessions"));
    adw_dialog_set_content_width(ADW_DIALOG(dialog_), 520);
    adw_dialog_set_content_height(ADW_DIALOG(dialog_), 600);

    auto *toolbar = ADW_TOOLBAR_VIEW(adw_toolbar_view_new());
    auto *header = ADW_HEADER_BAR(adw_header_bar_new());
    adw_toolbar_view_add_top_bar(toolbar, GTK_WIDGET(header));
    adw_dialog_set_child(ADW_DIALOG(dialog_), GTK_WIDGET(toolbar));

    dialog_stack_ = ADW_VIEW_STACK(adw_view_stack_new());
    adw_toolbar_view_set_content(toolbar, GTK_WIDGET(dialog_stack_));

    GtkWidget *setup = build_setup_page();
    GtkWidget *session = build_session_page();
    adw_view_stack_add_named(dialog_stack_, setup, "setup");
    adw_view_stack_add_named(dialog_stack_, session, "session");
    adw_view_stack_set_visible_child_name(dialog_stack_, "setup");
  }

  GtkWidget *build_setup_page()
  {
    auto *box = GTK_BOX(gtk_box_new(GTK_ORIENTATION_VERTICAL, 0));
    auto *mode_stack = ADW_VIEW_STACK(adw_view_stack_new());
    auto *switcher = ADW_VIEW_SWITCHER(adw_view_switcher_new());
    adw_view_switcher_set_stack(switcher, mode_stack);
    adw_view_switcher_set_policy(switcher, ADW_VIEW_SWITCHER_POLICY_WIDE);
    gtk_widget_set_halign(GTK_WIDGET(switcher), GTK_ALIGN_CENTER);
    gtk_widget_set_margin_top(GTK_WIDGET(switcher), 12);
    gtk_widget_set_margin_bottom(GTK_WIDGET(switcher), 6);
    gtk_box_append(box, GTK_WIDGET(switcher));
    gtk_widget_set_vexpand(GTK_WIDGET(mode_stack), TRUE);
    gtk_box_append(box, GTK_WIDGET(mode_stack));

    auto *host_page = ADW_PREFERENCES_PAGE(adw_preferences_page_new());
    auto *host_group = ADW_PREFERENCES_GROUP(adw_preferences_group_new());
    adw_preferences_group_set_title(host_group, _("Share This Page"));
    adw_preferences_group_set_description(
        host_group, _("People you allow can view the signed-in page through an encrypted session."));
    host_name_ = adw_entry_row_new();
    server_entry_ = adw_entry_row_new();
    adw_preferences_row_set_title(ADW_PREFERENCES_ROW(host_name_), _("Your Name"));
    adw_preferences_row_set_title(ADW_PREFERENCES_ROW(server_entry_), _("Session Server"));
    gtk_editable_set_text(GTK_EDITABLE(server_entry_), kDefaultServer);
    adw_preferences_group_add(host_group, host_name_);
    adw_preferences_group_add(host_group, server_entry_);
    host_button_ = gtk_button_new_with_mnemonic(_("_Start Sharing"));
    gtk_widget_add_css_class(host_button_, "suggested-action");
    gtk_widget_add_css_class(host_button_, "pill");
    gtk_widget_set_halign(host_button_, GTK_ALIGN_CENTER);
    gtk_widget_set_margin_top(host_button_, 12);
    adw_preferences_group_add(host_group, host_button_);
    adw_preferences_page_add(host_page, host_group);
    adw_view_stack_add_titled(mode_stack, GTK_WIDGET(host_page), "host", _("Share"));

    auto *join_page = ADW_PREFERENCES_PAGE(adw_preferences_page_new());
    auto *join_group = ADW_PREFERENCES_GROUP(adw_preferences_group_new());
    adw_preferences_group_set_title(join_group, _("Join a Session"));
    adw_preferences_group_set_description(
        join_group, _("Paste an invitation from someone sharing a page."));
    join_invitation_ = adw_entry_row_new();
    join_name_ = adw_entry_row_new();
    adw_preferences_row_set_title(ADW_PREFERENCES_ROW(join_invitation_), _("Invitation Link"));
    adw_preferences_row_set_title(ADW_PREFERENCES_ROW(join_name_), _("Your Name"));
    adw_preferences_group_add(join_group, join_invitation_);
    adw_preferences_group_add(join_group, join_name_);
    join_button_ = gtk_button_new_with_mnemonic(_("_Join Session"));
    gtk_widget_add_css_class(join_button_, "suggested-action");
    gtk_widget_add_css_class(join_button_, "pill");
    gtk_widget_set_halign(join_button_, GTK_ALIGN_CENTER);
    gtk_widget_set_margin_top(join_button_, 12);
    adw_preferences_group_add(join_group, join_button_);
    adw_preferences_page_add(join_page, join_group);
    adw_view_stack_add_titled(mode_stack, GTK_WIDGET(join_page), "join", _("Join"));

    auto *status_group = ADW_PREFERENCES_GROUP(adw_preferences_group_new());
    gtk_widget_set_margin_start(GTK_WIDGET(status_group), 12);
    gtk_widget_set_margin_end(GTK_WIDGET(status_group), 12);
    gtk_widget_set_margin_bottom(GTK_WIDGET(status_group), 12);
    status_row_ = adw_action_row_new();
    status_spinner_ = adw_spinner_new();
    gtk_widget_set_valign(status_spinner_, GTK_ALIGN_CENTER);
    adw_action_row_add_prefix(ADW_ACTION_ROW(status_row_), status_spinner_);
    gtk_widget_set_visible(status_row_, FALSE);
    adw_preferences_group_add(status_group, status_row_);
    gtk_box_append(box, GTK_WIDGET(status_group));

    g_signal_connect(host_name_, "changed", G_CALLBACK(setup_changed), this);
    g_signal_connect(server_entry_, "changed", G_CALLBACK(setup_changed), this);
    g_signal_connect(join_invitation_, "changed", G_CALLBACK(setup_changed), this);
    g_signal_connect(join_name_, "changed", G_CALLBACK(setup_changed), this);
    g_signal_connect(host_button_, "clicked", G_CALLBACK(host_clicked), this);
    g_signal_connect(join_button_, "clicked", G_CALLBACK(join_clicked), this);
    update_setup_actions();
    return GTK_WIDGET(box);
  }

  GtkWidget *build_session_page()
  {
    auto *page = ADW_PREFERENCES_PAGE(adw_preferences_page_new());
    auto *session_group = ADW_PREFERENCES_GROUP(adw_preferences_group_new());
    session_row_ = adw_action_row_new();
    adw_preferences_row_set_title(ADW_PREFERENCES_ROW(session_row_), _("Shared Session"));
    adw_preferences_group_add(session_group, session_row_);

    invitation_row_ = adw_entry_row_new();
    adw_preferences_row_set_title(ADW_PREFERENCES_ROW(invitation_row_), _("Invitation Link"));
    gtk_editable_set_editable(GTK_EDITABLE(invitation_row_), FALSE);
    copy_button_ = new_icon_button("edit-copy-symbolic", _("Copy Invitation"));
    gtk_widget_set_valign(copy_button_, GTK_ALIGN_CENTER);
    adw_entry_row_add_suffix(ADW_ENTRY_ROW(invitation_row_), copy_button_);
    adw_preferences_group_add(session_group, invitation_row_);
    g_signal_connect(copy_button_, "clicked", G_CALLBACK(copy_clicked), this);

    GtkWidget *stop = gtk_button_new_with_mnemonic(_("_Stop Session"));
    gtk_widget_add_css_class(stop, "destructive-action");
    gtk_widget_add_css_class(stop, "pill");
    gtk_widget_set_halign(stop, GTK_ALIGN_CENTER);
    gtk_widget_set_margin_top(stop, 12);
    adw_preferences_group_add(session_group, stop);
    g_signal_connect_swapped(stop, "clicked", G_CALLBACK(stop_clicked), this);

    participants_group_ = ADW_PREFERENCES_GROUP(adw_preferences_group_new());
    adw_preferences_group_set_title(participants_group_, _("People"));
    gtk_widget_set_visible(GTK_WIDGET(participants_group_), FALSE);
    adw_preferences_page_add(page, session_group);
    adw_preferences_page_add(page, participants_group_);
    return GTK_WIDGET(page);
  }

  void update_setup_actions()
  {
    gtk_widget_set_sensitive(host_button_,
                             !busy_ && has_text(GTK_EDITABLE(host_name_)) &&
                                 has_text(GTK_EDITABLE(server_entry_)));
    gtk_widget_set_sensitive(join_button_,
                             !busy_ && has_text(GTK_EDITABLE(join_invitation_)) &&
                                 has_text(GTK_EDITABLE(join_name_)));
  }

  void set_busy(bool busy)
  {
    busy_ = busy;
    update_setup_actions();
  }

  void ensure_host_banner()
  {
    if (!session_active_ || !host_ || host_banner_)
      return;
    g_autoptr(GObject) host = G_OBJECT(g_weak_ref_get(&host_window_));
    if (!host)
      return;

    host_banner_ = adw_banner_new(_("You’re sharing this page"));
    g_object_add_weak_pointer(G_OBJECT(host_banner_),
                              reinterpret_cast<gpointer *>(&host_banner_));
    adw_banner_set_button_label(ADW_BANNER(host_banner_), _("Stop Sharing"));
    adw_banner_set_revealed(ADW_BANNER(host_banner_), TRUE);
    g_signal_connect_swapped(host_banner_, "button-clicked",
                             G_CALLBACK(stop_clicked), this);
    ephy_window_set_shared_banner(EPHY_WINDOW(host), host_banner_);
  }

  void detach_host_banner()
  {
    g_autoptr(GObject) host = G_OBJECT(g_weak_ref_get(&host_window_));
    if (host && host_banner_)
      ephy_window_set_shared_banner(EPHY_WINDOW(host), nullptr);
    host_banner_ = nullptr;
  }

  void update_guest_banner()
  {
    if (!guest_banner_)
      return;
    adw_banner_set_title(ADW_BANNER(guest_banner_),
                         session_active_ ? _("You’re viewing a shared page")
                                         : _("Connecting to shared session…"));
    adw_banner_set_revealed(ADW_BANNER(guest_banner_), TRUE);
  }

  Interface *owner_;
  GtkApplication *application_;
  GWeakRef host_window_;

  GtkWidget *dialog_ = nullptr;
  AdwViewStack *dialog_stack_ = nullptr;
  GtkWidget *host_name_ = nullptr;
  GtkWidget *server_entry_ = nullptr;
  GtkWidget *join_invitation_ = nullptr;
  GtkWidget *join_name_ = nullptr;
  GtkWidget *host_button_ = nullptr;
  GtkWidget *join_button_ = nullptr;
  GtkWidget *status_row_ = nullptr;
  GtkWidget *status_spinner_ = nullptr;
  GtkWidget *session_row_ = nullptr;
  GtkWidget *invitation_row_ = nullptr;
  GtkWidget *copy_button_ = nullptr;
  AdwPreferencesGroup *participants_group_ = nullptr;
  std::unordered_map<std::string, GtkWidget *> participants_;

  AdwApplicationWindow *guest_window_ = nullptr;
  GtkWidget *guest_banner_ = nullptr;
  EphyEngineSession *guest_session_ = nullptr;
  EphyEngineView *guest_view_ = nullptr;
  GtkWidget *host_banner_ = nullptr;

  std::string invitation_;
  double local_zoom_ = 1.0;
  bool session_active_ = false;
  bool host_ = false;
  bool busy_ = false;
  bool clearing_ = false;
};

std::shared_ptr<Interface>
Interface::create(GtkApplication *application)
{
  g_return_val_if_fail(GTK_IS_APPLICATION(application), nullptr);
  return std::shared_ptr<Interface>(new Interface(application));
}

Interface::Interface(GtkApplication *application)
    : impl_(std::make_unique<Impl>(this, application))
{
}

Interface::~Interface() = default;

void Interface::present(GtkWindow *parent) { impl_->present(parent); }
void Interface::active(bool host, const std::string &title, const std::string &invitation)
{
  impl_->active(host, title, invitation);
}
void Interface::status(const std::string &text) { impl_->status(text); }
void Interface::attach_host(EphyWindow *window) { impl_->attach_host(window); }
EphyEngineView *Interface::create_guest() { return impl_->create_guest(); }
void Interface::participant(const std::string &id,
                            const std::string &name,
                            bool pending,
                            bool control)
{
  impl_->participant(id, name, pending, control);
}
void Interface::remove(const std::string &id) { impl_->remove(id); }
void Interface::ask_resize(const std::string &name,
                           int width,
                           int height,
                           std::function<void(bool)> reply)
{
  impl_->ask_resize(name, width, height, std::move(reply));
}
void Interface::clear() { impl_->clear(); }

}  // namespace GnosisShared
