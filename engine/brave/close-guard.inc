/* SPDX-License-Identifier: GPL-3.0-or-later */
bool Page::close_is_approved() const {
  // Chromium already completed unload before destroying this target.
  return state_->native_destroyed || (state_->close_approved && state_->close_navigation == state_->navigation_generation);
}
void Page::cancel_close() {
  auto& s = *state_;
  ++s.close_generation;
  if (s.close_dialog) {
    auto* dialog = std::exchange(s.close_dialog, nullptr);
    if (s.close_dialog_response) g_signal_handler_disconnect(dialog, s.close_dialog_response);
    s.close_dialog_response = 0;
    adw_dialog_force_close(dialog); g_object_unref(dialog);
  }
  const auto id = std::exchange(s.close_request, 0);
  if (s.close_watch) { g_source_remove(s.close_watch); s.close_watch = 0; }
  if (id && s.process) s.process->cancel_beforeunload(target, id);
  s.close_approved = false;
  if (s.close_prompted && s.script_dialog_open) dismiss_script_dialog(true);
  auto callback = std::move(s.close_callback); s.close_callback = {};
  const bool prompted = std::exchange(s.close_prompted, false);
  if (callback) callback(false, prompted, "");
}
void Page::request_close(std::function<void(bool, bool, const std::string&)> callback) {
  if (close_is_approved()) { callback(true, false, ""); return; }
  cancel_close();
  auto& s = *state_;
  s.close_navigation = s.navigation_generation;
  if (is_closed() || !s.attached || target.empty()) {
    s.close_approved = true; callback(true, false, ""); return;
  }
  const auto generation = s.close_generation;
  const auto navigation = s.navigation_generation;
  const auto owner = target;
  s.close_callback = std::move(callback); s.close_started = g_get_monotonic_time();
  s.close_request = s.process->check_beforeunload(owner,
    [weak = weak_from_this(), generation, navigation, owner](JsonObject* result, const std::string& error) {
      auto page = weak.lock();
      if (!page || page->state_->close_generation != generation) return;
      auto& s = *page->state_;
      s.close_request = 0;
      if (s.close_watch) { g_source_remove(s.close_watch); s.close_watch = 0; }
      bool approved = false;
      std::string failure = error;
      if (failure.empty() && result && page->target == owner && s.navigation_generation == navigation &&
          std::string(text(result,"target")) == owner) {
        auto* value = json_object_get_member(result,"proceed");
        if (value && json_node_get_value_type(value) == G_TYPE_BOOLEAN) approved = json_node_get_boolean(value);
        else failure = _("Invalid page close decision");
      } else if (failure.empty()) failure = _("The page changed during its close check");
      if (!failure.empty()) { page->show_close_failure(failure); return; }
      s.close_approved = approved; s.close_navigation = navigation;
      auto callback = std::move(s.close_callback); s.close_callback = {};
      if (callback) callback(approved, std::exchange(s.close_prompted, false), failure);
    });
  if (!s.close_callback || s.close_dialog) return;
  s.close_watch = g_timeout_add_full(G_PRIORITY_DEFAULT, 1000, +[](gpointer data) -> gboolean {
    auto page = static_cast<std::weak_ptr<Page>*>(data)->lock();
    if (!page || !page->state_->close_callback) return G_SOURCE_REMOVE;
    auto& s = *page->state_;
    // A visible native beforeunload dialog waits for the user, without a timer.
    if (s.script_dialog_open || g_get_monotonic_time() - s.close_started < 8 * G_USEC_PER_SEC) return G_SOURCE_CONTINUE;
    s.close_watch = 0;
    const auto id = std::exchange(s.close_request, 0);
    if (id) s.process->cancel_beforeunload(page->target, id);
    page->show_close_failure(_("The page did not respond to its close check"));
    return G_SOURCE_REMOVE;
  }, new std::weak_ptr<Page>(weak_from_this()), +[](gpointer data) { delete static_cast<std::weak_ptr<Page>*>(data); });
}

void Page::show_close_failure(const std::string& error) {
  auto& s = *state_;
  auto* root = gtk_widget_get_root(s.content);
  if (!s.close_callback) return;
  if (!root || !GTK_IS_WINDOW(root)) {
    auto callback = std::move(s.close_callback); s.close_callback = {};
    callback(false, std::exchange(s.close_prompted, false), error); return;
  }
  auto* dialog = adw_alert_dialog_new(_("Close This Page?"),
    _("Gnosis could not check whether this page can close. Closing it may discard unsaved changes."));
  s.close_dialog = ADW_DIALOG(g_object_ref_sink(dialog));
  adw_alert_dialog_add_response(ADW_ALERT_DIALOG(dialog), "cancel", _("_Keep Open"));
  adw_alert_dialog_add_response(ADW_ALERT_DIALOG(dialog), "accept", _("Close _Anyway"));
  adw_alert_dialog_set_default_response(ADW_ALERT_DIALOG(dialog), "cancel");
  adw_alert_dialog_set_close_response(ADW_ALERT_DIALOG(dialog), "cancel");
  adw_alert_dialog_set_response_appearance(ADW_ALERT_DIALOG(dialog), "accept", ADW_RESPONSE_DESTRUCTIVE);
  struct Response { std::weak_ptr<Page> page; guint64 generation, navigation; };
  s.close_dialog_response = g_signal_connect_data(dialog, "response", G_CALLBACK(+[](AdwAlertDialog*, const char* answer, gpointer data) {
    const auto response = *static_cast<Response*>(data);
    auto page = response.page.lock();
    if (!page || page->state_->close_generation != response.generation) return;
    auto& s = *page->state_;
    const bool approved = std::string(answer) == "accept" && s.navigation_generation == response.navigation;
    auto callback = std::move(s.close_callback); s.close_callback = {};
    page->cancel_close();
    s.close_navigation = response.navigation; s.close_approved = approved;
    if (callback) callback(approved, true, "");
  }), new Response{weak_from_this(),s.close_generation,s.navigation_generation},
    +[](gpointer data,GClosure*) { delete static_cast<Response*>(data); },G_CONNECT_DEFAULT);
  gtk_window_present(GTK_WINDOW(root));
  adw_dialog_present(dialog,GTK_WIDGET(root));
}
