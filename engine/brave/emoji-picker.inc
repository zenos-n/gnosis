/* SPDX-License-Identifier: GPL-3.0-or-later */
namespace {
using EmojiBounds = std::function<void(GdkRectangle)>;
bool emoji_box(JsonObject* result, const char* side, GdkRectangle& rect) {
  auto* model = object(result, "model");
  auto* quad = model && json_object_has_member(model, side) ? json_object_get_array_member(model, side) : nullptr;
  if (!quad || json_array_get_length(quad) != 8) return false;
  double left = G_MAXDOUBLE, top = G_MAXDOUBLE, right = -G_MAXDOUBLE, bottom = -G_MAXDOUBLE;
  for (guint i = 0; i < 8; i += 2) {
    const double x = json_array_get_double_element(quad, i), y = json_array_get_double_element(quad, i + 1);
    if (!std::isfinite(x) || !std::isfinite(y) || std::abs(x) > G_MAXINT / 4 || std::abs(y) > G_MAXINT / 4) return false;
    left = std::min(left, x); top = std::min(top, y); right = std::max(right, x); bottom = std::max(bottom, y);
  }
  rect = {int(std::floor(left)), int(std::floor(top)), int(std::ceil(right - left)), int(std::ceil(bottom - top))};
  return true;
}
// Chromium reports layout quads relative to each renderer's local root.
void emoji_frame_bounds(std::shared_ptr<Process> process, const std::string& source, GdkRectangle rect, EmojiBounds ready, unsigned depth = 0) {
  if (process->closing || depth > 32) return;
  const auto frame = process->frame_sessions.find(source);
  if (frame == process->frame_sessions.end()) { ready(rect); return; }
  const auto parent = frame->second.parent;
  process->command("DOM.getFrameOwner", "{\"frameId\":" + quote(frame->second.root_frame) + "}", parent,
    [process, source, parent, rect, ready, depth](JsonObject* owner, const std::string& error) {
      if (!error.empty() || !owner || !process->frame_sessions.contains(source)) return;
      const auto node = json_object_get_int_member_with_default(owner, "backendNodeId", 0);
      if (!node) return;
      process->command("DOM.getBoxModel", "{\"backendNodeId\":" + std::to_string(node) + "}", parent,
        [process, parent, rect, ready, depth](JsonObject* result, const std::string& error) mutable {
          GdkRectangle frame;
          if (!error.empty() || !emoji_box(result, "content", frame)) return;
          const double x = double(rect.x) + frame.x, y = double(rect.y) + frame.y;
          if (std::abs(x) > G_MAXINT / 4 || std::abs(y) > G_MAXINT / 4) return;
          rect.x = int(x); rect.y = int(y);
          emoji_frame_bounds(process, parent, rect, ready, depth + 1);
        });
    });
}
void emoji_field_bounds(std::shared_ptr<Process> process, const std::string& source, const std::string& insertion, EmojiBounds ready) {
  process->command("Runtime.callFunctionOn", "{\"objectId\":" + quote(insertion) +
    ",\"functionDeclaration\":\"function() { return this.field; }\"}", source,
    [process, source, ready](JsonObject* result, const std::string& error) {
      const std::string node = text(object(result, "result"), "objectId");
      if (!error.empty() || node.empty()) return;
      process->command("DOM.getBoxModel", "{\"objectId\":" + quote(node) + "}", source,
        [process, source, ready](JsonObject* result, const std::string& error) {
          GdkRectangle rect;
          if (!error.empty() || !emoji_box(result, "border", rect)) return;
          emoji_frame_bounds(process, source, rect, ready);
        });
    });
}
}
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
    insert.field = field;
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
            (page->state_->surface ? focus != page->state_->surface : !gtk_widget_is_ancestor(focus, page->widget()))) return;
        auto* remote = object(result, "result");
        if (std::string(text(remote, "type")) != "function") return;
        const std::string insertion = text(remote, "objectId");
        if (insertion.empty()) return;
        emoji_field_bounds(page->state_->process, source, insertion, [weak, generation, context, unique, source, insertion](GdkRectangle point) {
          auto page = weak.lock();
          if (!page || page->is_closed() || !page->state_->mapped || generation != page->state_->emoji_generation || page->state_->emoji_picker) return;
          const auto found = page->state_->menu_contexts.find(context);
          auto* root = gtk_widget_get_root(page->widget());
          if (found == page->state_->menu_contexts.end() || found->second != unique || !root ||
              (page->state_->surface && gtk_root_get_focus(root) != page->state_->surface)) return;
          auto* picker = gtk_emoji_chooser_new();
          page->state_->emoji_picker = picker;
          page->state_->emoji_context = context;
          gtk_widget_set_parent(picker, page->widget());
          gtk_popover_set_position(GTK_POPOVER(picker), GTK_POS_BOTTOM);
          const double zoom = page->state_->zoom_applied;
          const double width = gtk_widget_get_width(page->widget()), height = gtk_widget_get_height(page->widget());
          const int left = int(std::clamp(point.x * zoom, 0.0, width)), top = int(std::clamp(point.y * zoom, 0.0, height));
          point = {left, top, std::max(1, int(std::clamp((double(point.x) + point.width) * zoom, 0.0, width)) - left),
                             std::max(1, int(std::clamp((double(point.y) + point.height) * zoom, 0.0, height)) - top)};
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
      });
  }
}
