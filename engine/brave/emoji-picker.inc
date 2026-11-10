/* SPDX-License-Identifier: GPL-3.0-or-later */
void Page::dismiss_emoji_picker() {
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
}
void Page::show_emoji_picker() {
  if (is_closed() || !state_->mapped || !state_->attached) return;
  dismiss_emoji_picker();
  const auto generation = state_->emoji_generation;
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
    return text => {
      if (!field.isConnected || active() !== field || field.disabled || field.readOnly) return;
      field.focus({preventScroll:true});
      if (start !== null) field.setSelectionRange(start, end, direction);
      if (range) {
        if (!field.contains(range.commonAncestorContainer)) return;
        const s = getSelection(); s.removeAllRanges(); s.addRange(range);
      }
      document.execCommand('insertText', false, text);
    };
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
            (page->state_->surface ? focus != page->state_->surface : !gtk_widget_is_ancestor(focus, page->widget()))) return;
        auto* remote = object(result, "result");
        if (std::string(text(remote, "type")) != "function") return;
        const std::string insertion = text(remote, "objectId");
        if (insertion.empty()) return;
        auto* picker = gtk_emoji_chooser_new();
        page->state_->emoji_picker = picker;
        page->state_->emoji_context = context;
        gtk_widget_set_parent(picker, page->widget());
        gtk_popover_set_position(GTK_POPOVER(picker), GTK_POS_BOTTOM);
        GdkRectangle point{gtk_widget_get_width(page->widget()) / 2, gtk_widget_get_height(page->widget()) / 2, 1, 1};
        gtk_popover_set_pointing_to(GTK_POPOVER(picker), &point);
        using Pick = std::function<void(const char*)>;
        auto* pick = new Pick([weak, generation, source, insertion](const char* emoji) {
          auto page = weak.lock();
          if (!page || page->is_closed() || !page->state_->mapped || generation != page->state_->emoji_generation) return;
          page->focus();
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
              page->dismiss_emoji_picker(); page->focus();
            }
            return G_SOURCE_REMOVE;
          }, new Cleanup{page->weak_from_this(), page->state_->emoji_generation},
          +[](gpointer data) { delete static_cast<Cleanup*>(data); });
        })), page.get());
        gtk_popover_popup(GTK_POPOVER(picker));
      });
  }
}
