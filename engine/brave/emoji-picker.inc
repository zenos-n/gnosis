/* SPDX-License-Identifier: GPL-3.0-or-later */
void Page::dismiss_emoji_picker(bool restore_focus) {
  const auto group = "gnosis-emoji-" + std::to_string(state_->emoji_generation++);
  for (const auto& source : state_->emoji_sessions)
    if (state_->process && !state_->process->closing)
      state_->process->command("Runtime.releaseObjectGroup", "{\"objectGroup\":" + quote(group) + "}", source);
  state_->emoji_sessions.clear();
  state_->emoji_context = 0;
  if (auto* picker = std::exchange(state_->emoji_picker, nullptr)) {
    g_signal_handlers_disconnect_by_data(picker, this);
    gtk_popover_popdown(GTK_POPOVER(picker));
    gtk_widget_unparent(picker);
  }
  if (restore_focus && state_->mapped) {
    auto* root = gtk_widget_get_root(widget());
    if (root && GTK_IS_WINDOW(root) && gtk_window_is_active(GTK_WINDOW(root))) focus();
  }
  if (state_->surface) gnosis_compositor_view_hold_caret(GNOSIS_COMPOSITOR_VIEW(state_->surface), false);
}
void Page::show_emoji_picker() {
  if (is_closed() || !state_->mapped || !state_->attached) return;
  dismiss_emoji_picker();
  const auto generation = state_->emoji_generation;
  using Begin = std::function<gboolean()>;
  auto* begin = new Begin([weak = weak_from_this(), generation, deadline = g_get_monotonic_time() + 200000]() -> gboolean {
    auto page = weak.lock();
    if (!page || page->is_closed() || !page->state_->mapped || generation != page->state_->emoji_generation || !page->state_->surface) return G_SOURCE_REMOVE;
    auto* root = gtk_widget_get_root(page->widget());
    if (!root || !GTK_IS_WINDOW(root) || !gtk_window_is_active(GTK_WINDOW(root)) ||
        gtk_root_get_focus(root) != page->state_->surface) return G_SOURCE_REMOVE;
    GdkRectangle caret;
    if (!gnosis_compositor_view_caret_rect(GNOSIS_COMPOSITOR_VIEW(page->state_->surface), &caret))
      return g_get_monotonic_time() < deadline ? G_SOURCE_CONTINUE : G_SOURCE_REMOVE;
    page->capture_emoji_picker(generation);
    return G_SOURCE_REMOVE;
  });
  if ((*begin)() == G_SOURCE_CONTINUE)
    g_timeout_add_full(G_PRIORITY_DEFAULT, 5, +[](gpointer data) -> gboolean { return (*static_cast<Begin*>(data))(); },
      begin, +[](gpointer data) { delete static_cast<Begin*>(data); });
  else delete begin;
}
void Page::capture_emoji_picker(guint64 generation) {
  const auto group = "gnosis-emoji-" + std::to_string(generation);
  // Capture the original editor and selection in an isolated world, only on demand.
  const std::string capture = R"JS((() => {
    if (!document.hasFocus()) return;
    const active = () => { let e = document.activeElement; while (e?.shadowRoot?.activeElement) e = e.shadowRoot.activeElement; return e; };
    const field = active();
    const input = field instanceof HTMLInputElement;
    const control = input || field instanceof HTMLTextAreaElement;
    if (!field || field.disabled || field.readOnly ||
        (input && !['text','search','url','tel','email','password'].includes(field.type)) ||
        (!control && !field.isContentEditable)) return;
    const start = control ? field.selectionStart : null, end = control ? field.selectionEnd : null;
    const direction = control ? field.selectionDirection : null;
    const selection = getSelection();
    const range = !control && selection?.rangeCount ? selection.getRangeAt(0).cloneRange() : null;
    const insert = text => {
      if (!field.isConnected || active() !== field || field.disabled || field.readOnly) return;
      field.focus({preventScroll:true});
      if (start !== null) field.setSelectionRange(start, end, direction);
      if (range) {
        if (!field.contains(range.commonAncestorContainer)) return;
        const s = getSelection(); s.removeAllRanges(); s.addRange(range);
      }
      document.execCommand('insertText', false, text);
    };
    return insert;
  })())JS";
  for (const auto& [context, unique] : state_->menu_contexts) {
    const auto source = state_->menu_sessions.find(context);
    if (source == state_->menu_sessions.end()) continue;
    state_->emoji_sessions.insert(source->second);
    state_->process->command("Runtime.evaluate", "{\"uniqueContextId\":" + quote(unique) +
      ",\"objectGroup\":" + quote(group) + ",\"expression\":" + quote(capture) + "}", source->second,
      [weak = weak_from_this(), generation, context, unique, source = source->second](JsonObject* result, const std::string& error) {
        auto page = weak.lock();
        if (!page || page->is_closed() || !page->state_->mapped || generation != page->state_->emoji_generation ||
            page->state_->emoji_picker || !error.empty()) return;
        const auto found = page->state_->menu_contexts.find(context);
        auto* root = gtk_widget_get_root(page->widget());
        auto* focus = root ? gtk_root_get_focus(root) : nullptr;
        if (found == page->state_->menu_contexts.end() || found->second != unique || !focus ||
            (page->state_->surface ? focus != page->state_->surface : !gtk_widget_is_ancestor(focus, page->widget())) ||
            (GTK_IS_WINDOW(root) && !gtk_window_is_active(GTK_WINDOW(root)))) return;
        auto* remote = object(result, "result");
        if (std::string(text(remote, "type")) != "function") return;
        const std::string insertion = text(remote, "objectId");
        if (insertion.empty()) return;
        GdkRectangle point;
        if (!page->state_->surface || !GNOSIS_IS_COMPOSITOR_VIEW(page->state_->surface) ||
            !gnosis_compositor_view_caret_rect(GNOSIS_COMPOSITOR_VIEW(page->state_->surface), &point)) return;
        auto* picker = gtk_emoji_chooser_new();
        page->state_->emoji_picker = picker;
        page->state_->emoji_context = context;
        gnosis_compositor_view_hold_caret(GNOSIS_COMPOSITOR_VIEW(page->state_->surface), true);
        gtk_widget_set_parent(picker, page->widget());
        gtk_popover_set_position(GTK_POPOVER(picker), GTK_POS_BOTTOM);
        const double width = gtk_widget_get_width(page->widget()), height = gtk_widget_get_height(page->widget());
        const int left = int(std::clamp(double(point.x), 0.0, width)), top = int(std::clamp(double(point.y), 0.0, height));
        point = {left, top, std::max(1, int(std::clamp((double(point.x) + point.width), 0.0, width)) - left),
                           std::max(1, int(std::clamp((double(point.y) + point.height), 0.0, height)) - top)};
        gtk_popover_set_pointing_to(GTK_POPOVER(picker), &point);
        using Pick = std::function<void(const char*)>;
        auto* pick = new Pick([weak, generation, source, insertion](const char* emoji) {
          auto page = weak.lock();
          if (!page || page->is_closed() || !page->state_->mapped || generation != page->state_->emoji_generation) return;
          page->state_->process->command("Runtime.callFunctionOn", "{\"objectId\":" + quote(insertion) +
            ",\"functionDeclaration\":\"function(text) { this(text); }\",\"arguments\":[{\"value\":" + quote(emoji) + "}]}", source);
          gtk_popover_popdown(GTK_POPOVER(page->state_->emoji_picker));
        });
        g_signal_connect_data(picker, "emoji-picked", G_CALLBACK(+[](GtkEmojiChooser*, const char* emoji, gpointer data) {
          auto invoke = *static_cast<Pick*>(data); invoke(emoji);
        }), pick, +[](gpointer data, GClosure*) { delete static_cast<Pick*>(data); }, G_CONNECT_DEFAULT);
        g_signal_connect(picker, "closed", G_CALLBACK((+[](GtkPopover*, gpointer data) {
          auto* page = static_cast<Page*>(data);
          using Cleanup = std::pair<std::weak_ptr<Page>, guint64>;
          g_idle_add_full(G_PRIORITY_DEFAULT_IDLE, +[](gpointer data) -> gboolean {
            const auto& [weak, generation] = *static_cast<Cleanup*>(data);
            if (auto page = weak.lock(); page && generation == page->state_->emoji_generation) {
              page->dismiss_emoji_picker(true);
            }
            return G_SOURCE_REMOVE;
          }, new Cleanup{page->weak_from_this(), page->state_->emoji_generation},
          +[](gpointer data) { delete static_cast<Cleanup*>(data); });
        })), page.get());
        gtk_popover_popup(GTK_POPOVER(picker));
      });
  }
}
