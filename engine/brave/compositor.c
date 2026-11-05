/* SPDX-License-Identifier: GPL-3.0-or-later
 * An embedding Wayland compositor whose surfaces are GTK render nodes.
 * No foreign toplevels, screen capture, or browser input via DevTools.
 */
#define WLR_USE_UNSTABLE
#include "compositor.h"
#include "outer-pointer.h"
#include "clipboard-bridge.h"
#include "wayland-source.h"
#include "edge-swipe.h"
#include <glib-unix.h>
#include <fcntl.h>
#include <unistd.h>
#include <linux/input-event-codes.h>
#include <drm_fourcc.h>
#include <wayland-server-core.h>
#include <wlr/backend.h>
#include <wlr/backend/headless.h>
#include <wlr/render/gles2.h>
#include <wlr/render/wlr_renderer.h>
#include <wlr/interfaces/wlr_keyboard.h>
#include <wlr/types/wlr_buffer.h>
#include <wlr/types/wlr_compositor.h>
#include <wlr/types/wlr_cursor_shape_v1.h>
#include <wlr/types/wlr_data_device.h>
#include <wlr/types/wlr_linux_dmabuf_v1.h>
#include <wlr/types/wlr_output.h>
#include <wlr/types/wlr_output_layout.h>
#include <wlr/types/wlr_pointer_constraints_v1.h>
#include <wlr/types/wlr_presentation_time.h>
#include <wlr/types/wlr_relative_pointer_v1.h>
#include <wlr/types/wlr_seat.h>
#include <wlr/types/wlr_subcompositor.h>
#include <wlr/types/wlr_viewporter.h>
#include <wlr/types/wlr_xdg_output_v1.h>
#include <wlr/types/wlr_xdg_shell.h>
#include <wlr/types/wlr_xdg_decoration_v1.h>
#include <wlr/util/log.h>
#include <xkbcommon/xkbcommon.h>
#include <xkbcommon/xkbcommon-names.h>

typedef struct {
  struct wl_list link;
  GnosisCompositorView *host;
  GnosisCompositor *server;
  struct wlr_xdg_toplevel *toplevel;
  struct wlr_xdg_toplevel_decoration_v1 *decoration;
  struct wl_listener decoration_destroy;
  struct wl_listener commit, map, unmap, destroy, maximize, fullscreen;
  int requested_width, requested_height;
  GWeakRef fullscreen_window;
  gboolean owns_fullscreen;
  gboolean content_fullscreen;
} View;
typedef struct {
  GnosisCompositor *server;
  struct wlr_surface *surface;
  struct wl_listener commit, destroy;
} Surface;
typedef struct {
  struct wlr_xdg_popup *popup;
  struct wl_listener commit, reposition, unmap, destroy;
} Popup;
typedef struct {
  struct wlr_presentation_feedback *feedback;
  gint64 frame;
} PresentedFrame;
typedef struct DesktopDrag DesktopDrag;
typedef struct ImportedDrag ImportedDrag;
struct _GnosisCompositor {
  GObject parent;
  struct wl_display *display;
  struct wlr_backend *backend;
  struct wlr_renderer *renderer;
  struct wlr_output *output;
  struct wlr_seat *seat;
  GnosisClipboardBridge *clipboard;
  DesktopDrag *export_drag;
  ImportedDrag *import_drag;
  struct wlr_pointer_constraints_v1 *constraints;
  struct wlr_relative_pointer_manager_v1 *relative;
  struct wlr_keyboard keyboard;
  struct wl_list views;
  GPtrArray *hosts; /* weak pointers; each widget owns a server reference */
  struct wl_listener new_constraint, new_toplevel, new_popup, new_surface, decoration, request_drag;
  struct wl_listener request_cursor, request_cursor_shape;
  struct wlr_surface *cursor_surface;
  int cursor_hotspot_x, cursor_hotspot_y;
  guint dispatch, touch_frame;
  char *directory, *socket;
  int drm_fd;
  gboolean keyboard_ready, shutting_down;
  uint32_t next_touch_id;
};
struct _GnosisCompositorView {
  GtkWidget parent;
  GnosisCompositor *server;
  char *token;
  View *active;
  struct wlr_pointer_constraint_v1 *constraint;
  struct wl_listener constraint_destroy;
  GnosisOuterPointer *outer_pointer;
  GdkDevice *pointer_device;
  GQueue presentations;
  gint64 last_reported_interval;
  gint64 trace_start;
  guint trace_ticks;
  double context_x, context_y;
  gint64 context_time;
  guint pressed_buttons;
  guint scroll_axes;
  GHashTable *touches;
  GnosisEdgeSwipe edge_swipe;
  gboolean navigation_gestures;
  GdkTexture *swipe_source, *swipe_destination;
  double swipe_progress, swipe_start_progress, swipe_end_progress;
  gint64 swipe_started, swipe_duration, swipe_deadline;
  gint64 swipe_ready_frame;
  int swipe_direction;
  guint swipe_animation;
  gboolean swipe_loading, swipe_loaded;
  gboolean captured, capture_inhibited, logged_gpu, logged_shm, vim_field;
};
G_DEFINE_TYPE(GnosisCompositor, gnosis_compositor, G_TYPE_OBJECT)
G_DEFINE_TYPE(GnosisCompositorView, gnosis_compositor_view, GTK_TYPE_WIDGET)
static void restore_fullscreen(View *view);
static void configure(View *view);
static void discard_presentations(GnosisCompositorView *self);
static void cancel_view_drag(GnosisCompositorView *self);
static void setup_drop_target(GnosisCompositorView *self);
static void request_drag(struct wl_listener *listener, void *data);
static GnosisCompositorView *pointer_host(GnosisCompositor *server);
static void update_cursor(GnosisCompositor *server);
static void cancel_touches(GnosisCompositorView *self);
static void cancel_swipe(GnosisCompositorView *self);
static void update_swipe(GnosisCompositorView *self, double x);
static void end_swipe(GnosisCompositorView *self, int direction);

static void listen(struct wl_signal *signal, struct wl_listener *listener, wl_notify_func_t callback) {
  listener->notify = callback; wl_signal_add(signal, listener);
}
static void stop_capture(GnosisCompositorView *self) {
  struct wlr_pointer_constraint_v1 *constraint = self->constraint;
  gboolean was_captured = self->captured;
  self->constraint = NULL; self->captured = FALSE;
  wl_list_remove(&self->constraint_destroy.link); wl_list_init(&self->constraint_destroy.link);
  gnosis_outer_pointer_release(self->outer_pointer);
  if (constraint && was_captured) wlr_pointer_constraint_v1_send_deactivated(constraint);
  if (self->server && self->server->display) wl_display_flush_clients(self->server->display);
}
static void capture_state(gpointer data, gboolean locked) {
  GnosisCompositorView *self = data;
  if (!locked) { self->capture_inhibited = TRUE; stop_capture(self); return; }
  if (!self->constraint) { gnosis_outer_pointer_release(self->outer_pointer); return; }
  self->captured = TRUE;
  wlr_pointer_constraint_v1_send_activated(self->constraint);
  wl_display_flush_clients(self->server->display);
  g_message("Wayland pointer locked: GTK cursor hidden, native relative motion enabled");
}
static void relative_motion(gpointer data, guint64 time, double dx, double dy, double raw_dx, double raw_dy) {
  GnosisCompositorView *self = data;
  if (!self->captured || !self->constraint) return;
  wlr_relative_pointer_manager_v1_send_relative_motion(self->server->relative, self->server->seat, time, dx, dy, raw_dx, raw_dy);
  wl_display_flush_clients(self->server->display);
}
static void constraint_destroy(struct wl_listener *listener, void *data) {
  (void)data; GnosisCompositorView *self = wl_container_of(listener, self, constraint_destroy);
  self->constraint = NULL; stop_capture(self);
}
static void try_capture(GnosisCompositorView *self) {
  if (self->constraint || self->capture_inhibited || !self->server->constraints || !self->active || !gtk_widget_has_focus(GTK_WIDGET(self))) return;
  struct wlr_surface *focused = self->server->seat->pointer_state.focused_surface;
  if (!focused) return;
  struct wlr_pointer_constraint_v1 *constraint = wlr_pointer_constraints_v1_constraint_for_surface(self->server->constraints, focused, self->server->seat);
  if (!constraint || constraint->type != WLR_POINTER_CONSTRAINT_V1_LOCKED) return;
  if (!pixman_region32_contains_point(&constraint->region, self->server->seat->pointer_state.sx, self->server->seat->pointer_state.sy, NULL)) return;
  GError *error = NULL;
  if (!self->outer_pointer) self->outer_pointer = gnosis_outer_pointer_new(GTK_WIDGET(self), capture_state, relative_motion, self, &error);
  if (!self->outer_pointer) {
    g_warning("Pointer capture unavailable: %s", error->message); g_error_free(error); return;
  }
  self->constraint = constraint;
  listen(&constraint->events.destroy, &self->constraint_destroy, constraint_destroy);
  if (!gnosis_outer_pointer_lock(self->outer_pointer, &error)) {
    g_warning("Pointer capture failed: %s", error ? error->message : "unmapped GTK widget");
    g_clear_error(&error); stop_capture(self);
  }
}
static void new_constraint(struct wl_listener *listener, void *data) {
  (void)data; GnosisCompositor *server = wl_container_of(listener, server, new_constraint);
  for (guint i = 0; i < server->hosts->len; i++) try_capture(g_ptr_array_index(server->hosts, i));
}
static void focus_left(GtkEventControllerFocus *controller, GnosisCompositorView *self) {
  (void)controller;
  cancel_swipe(self);
  cancel_touches(self);
  self->capture_inhibited = TRUE;
  stop_capture(self);
  if (self->server && self->server->seat && self->active &&
      self->server->seat->keyboard_state.focused_surface == self->active->toplevel->base->surface) {
    struct wlr_keyboard *keyboard = &self->server->keyboard;
    // The matching release may go to the address entry or another application.
    // Retire the old page's held keys before its keyboard focus leaves.
    while (keyboard->num_keycodes) {
      struct wlr_keyboard_key_event event = {.time_msec = (uint32_t)(g_get_monotonic_time() / 1000),
        .keycode = keyboard->keycodes[keyboard->num_keycodes - 1], .update_state = TRUE,
        .state = WL_KEYBOARD_KEY_STATE_RELEASED};
      wlr_keyboard_notify_key(keyboard, &event);
      wlr_seat_keyboard_notify_key(self->server->seat, event.time_msec, event.keycode, event.state);
    }
    wlr_seat_keyboard_notify_modifiers(self->server->seat, &keyboard->modifiers);
    wlr_seat_keyboard_notify_clear_focus(self->server->seat);
  }
}
static void sync_modifiers(GtkEventController *controller, GnosisCompositorView *self) {
  if (!self->server) return;
  GdkModifierType state = gtk_event_controller_get_current_event_state(controller);
  struct wlr_keyboard *keyboard = &self->server->keyboard;
  xkb_mod_mask_t depressed = 0, locked = 0;
  const struct { GdkModifierType flag; const char *name; } map[] = {
    {GDK_SHIFT_MASK, XKB_MOD_NAME_SHIFT}, {GDK_CONTROL_MASK, XKB_MOD_NAME_CTRL},
    {GDK_ALT_MASK, XKB_MOD_NAME_ALT}, {GDK_SUPER_MASK, XKB_MOD_NAME_LOGO},
  };
  for (guint i = 0; i < G_N_ELEMENTS(map); i++) {
    xkb_mod_index_t index = xkb_keymap_mod_get_index(keyboard->keymap, map[i].name);
    if ((state & map[i].flag) && index != XKB_MOD_INVALID) depressed |= (xkb_mod_mask_t)1 << index;
  }
  xkb_mod_index_t caps = xkb_keymap_mod_get_index(keyboard->keymap, XKB_MOD_NAME_CAPS);
  if ((state & GDK_LOCK_MASK) && caps != XKB_MOD_INVALID) locked |= (xkb_mod_mask_t)1 << caps;
  wlr_keyboard_notify_modifiers(keyboard, depressed, 0, locked, keyboard->modifiers.group);
  wlr_seat_keyboard_notify_modifiers(self->server->seat, &keyboard->modifiers);
}
static void focus_entered(GtkEventControllerFocus *controller, GnosisCompositorView *self) {
  (void)controller;
  if (!self->server || !self->active || !self->active->toplevel->base->surface->mapped) return;
  wlr_xdg_toplevel_set_activated(self->active->toplevel, TRUE);
  wlr_seat_keyboard_notify_enter(self->server->seat, self->active->toplevel->base->surface,
    self->server->keyboard.keycodes, self->server->keyboard.num_keycodes, &self->server->keyboard.modifiers);
}
static void unmapped(GtkWidget *widget, GnosisCompositorView *self) {
  cancel_swipe(self);
  cancel_touches(self);
  cancel_view_drag(self);
  gnosis_compositor_view_release_buttons(self);
  (void)widget; self->capture_inhibited = TRUE; stop_capture(self);
  discard_presentations(self);
  if (self->active) restore_fullscreen(self->active);
}
typedef struct { struct wlr_buffer *buffer; GnosisCompositor *server; } BufferLease;
static gboolean release_buffer(gpointer data) {
  BufferLease *lease = data;
  wlr_buffer_unlock(lease->buffer); g_object_unref(lease->server); g_free(lease);
  return G_SOURCE_REMOVE;
}
static void release_texture(gpointer data) {
  // GTK may release the texture from its render thread; wlroots stays on GLib's thread.
  // invoke() can run inline on a thread that acquires an unowned context.
  // A source always defers the release to GTK's main-loop dispatch.
  g_idle_add_full(G_PRIORITY_DEFAULT, release_buffer, data, NULL);
}
static GdkTexture *surface_texture(GnosisCompositorView *self, struct wlr_surface *surface) {
  if (!surface->buffer || !surface->buffer->source) return NULL;
  struct wlr_buffer *source = surface->buffer->source;
  struct wlr_dmabuf_attributes dmabuf;
  if (wlr_buffer_get_dmabuf(source, &dmabuf)) {
    GdkDmabufTextureBuilder *builder = gdk_dmabuf_texture_builder_new();
    gdk_dmabuf_texture_builder_set_display(builder, gtk_widget_get_display(GTK_WIDGET(self)));
    gdk_dmabuf_texture_builder_set_width(builder, dmabuf.width);
    gdk_dmabuf_texture_builder_set_height(builder, dmabuf.height);
    gdk_dmabuf_texture_builder_set_fourcc(builder, dmabuf.format);
    gdk_dmabuf_texture_builder_set_modifier(builder, dmabuf.modifier);
    gdk_dmabuf_texture_builder_set_n_planes(builder, dmabuf.n_planes);
    gdk_dmabuf_texture_builder_set_premultiplied(builder, TRUE);
    for (int i = 0; i < dmabuf.n_planes; i++) {
      gdk_dmabuf_texture_builder_set_fd(builder, i, dmabuf.fd[i]);
      gdk_dmabuf_texture_builder_set_offset(builder, i, dmabuf.offset[i]);
      gdk_dmabuf_texture_builder_set_stride(builder, i, dmabuf.stride[i]);
    }
    GError *error = NULL;
    BufferLease *lease = g_new0(BufferLease, 1);
    lease->buffer = wlr_buffer_lock(source); lease->server = g_object_ref(self->server);
    GdkTexture *texture = gdk_dmabuf_texture_builder_build(builder, release_texture, lease, &error);
    g_object_unref(builder);
    if (!texture) {
      release_buffer(lease);
      g_warning("Wayland buffer import failed: %s", error->message); g_error_free(error);
    } else if (!self->logged_gpu) {
      self->logged_gpu = TRUE;
      g_message("Wayland client GPU buffer imported directly into GTK: %dx%d, modifier=%" G_GUINT64_FORMAT,
                dmabuf.width, dmabuf.height, dmabuf.modifier);
    }
    return texture;
  }
  // Wayland also permits SHM buffers, particularly for small UI/cursor surfaces.
  void *pixels; uint32_t format; size_t stride;
  if (!wlr_buffer_begin_data_ptr_access(source, WLR_BUFFER_DATA_PTR_ACCESS_READ, &pixels, &format, &stride)) return NULL;
  GdkMemoryFormat memory;
  if (format == DRM_FORMAT_ARGB8888) memory = GDK_MEMORY_B8G8R8A8_PREMULTIPLIED;
  else if (format == DRM_FORMAT_XRGB8888) memory = GDK_MEMORY_B8G8R8X8;
  else { wlr_buffer_end_data_ptr_access(source); return NULL; }
  GBytes *bytes = g_bytes_new(pixels, stride * source->height);
  GdkTexture *texture = gdk_memory_texture_new(source->width, source->height, memory, bytes, stride);
  g_bytes_unref(bytes); wlr_buffer_end_data_ptr_access(source);
  if (!self->logged_shm) { self->logged_shm = TRUE; g_message("Wayland client supplied a shared-memory buffer"); }
  return texture;
}
typedef struct { GnosisCompositorView *host; GtkSnapshot *snapshot; } Snapshot;
static void snapshot_surface(struct wlr_surface *surface, int x, int y, void *data) {
  Snapshot *s = data;
  if (surface->current.transform != WL_OUTPUT_TRANSFORM_NORMAL) return;
  GdkTexture *texture = surface_texture(s->host, surface); if (!texture) return;
  struct wlr_fbox source; wlr_surface_get_buffer_source_box(surface, &source);
  if (source.width <= 0 || source.height <= 0) { g_object_unref(texture); return; }
  double sx = surface->current.width / source.width, sy = surface->current.height / source.height;
  struct wlr_box geometry = s->host->active->toplevel->base->geometry;
  float px = x - geometry.x, py = y - geometry.y;
  graphene_rect_t clip = GRAPHENE_RECT_INIT(px, py, surface->current.width, surface->current.height);
  graphene_rect_t bounds = GRAPHENE_RECT_INIT(px - source.x * sx, py - source.y * sy,
    gdk_texture_get_width(texture) * sx, gdk_texture_get_height(texture) * sy);
  gtk_snapshot_push_clip(s->snapshot, &clip);
  gtk_snapshot_append_texture(s->snapshot, texture, &bounds);
  gtk_snapshot_pop(s->snapshot); g_object_unref(texture);
  struct wlr_presentation_feedback *feedback = wlr_presentation_surface_sampled(surface);
  if (feedback) {
    PresentedFrame *pending = g_new0(PresentedFrame, 1);
    pending->feedback = feedback;
    pending->frame = gdk_frame_clock_get_frame_counter(gtk_widget_get_frame_clock(GTK_WIDGET(s->host)));
    g_queue_push_tail(&s->host->presentations, pending);
  }
}
static void snapshot_live(GtkWidget *widget, GtkSnapshot *snapshot) {
  GnosisCompositorView *self = GNOSIS_COMPOSITOR_VIEW(widget);
  if (!self->active || !self->active->toplevel->base->surface->mapped) return;
  Snapshot state = {self, snapshot};
  struct wlr_box geometry = self->active->toplevel->base->geometry;
  gtk_snapshot_save(snapshot);
  if (geometry.width > 0 && geometry.height > 0)
    gtk_snapshot_scale(snapshot, (float)gtk_widget_get_width(widget) / geometry.width,
                       (float)gtk_widget_get_height(widget) / geometry.height);
  wlr_xdg_surface_for_each_surface(self->active->toplevel->base, snapshot_surface, &state);
  gtk_snapshot_restore(snapshot);
}
#include "history-swipe.inc"
static void snapshot(GtkWidget *widget, GtkSnapshot *snapshot) {
  GnosisCompositorView *self = GNOSIS_COMPOSITOR_VIEW(widget);
  if (self->swipe_source) snapshot_swipe(self, snapshot);
  else snapshot_live(widget, snapshot);
}
static void frame_surface(struct wlr_surface *surface, int x, int y, void *data) {
  (void)x; (void)y; wlr_surface_send_frame_done(surface, data);
}
static void finish_presentations(GnosisCompositorView *self, GdkFrameClock *clock) {
  while (!g_queue_is_empty(&self->presentations)) {
    PresentedFrame *pending = g_queue_peek_head(&self->presentations);
    GdkFrameTimings *timings = gdk_frame_clock_get_timings(clock, pending->frame);
    if (timings && !gdk_frame_timings_get_complete(timings)) break;
    gint64 time = timings ? gdk_frame_timings_get_presentation_time(timings) : 0;
    gint64 interval = timings ? gdk_frame_timings_get_refresh_interval(timings) : 0;
    if (time > 0) {
      struct wlr_presentation_event event = {
        .output = self->server->output,
        .tv_sec = time / G_USEC_PER_SEC, .tv_nsec = (time % G_USEC_PER_SEC) * 1000,
        .refresh = interval > 0 ? (uint32_t)(interval * 1000) : 0,
        .seq = (uint64_t)pending->frame,
        // GTK supplies presentation timing. Do not claim hardware clock,
        // direct scanout, or zero-copy guarantees for the nested surface.
        .flags = 1, /* WP_PRESENTATION_FEEDBACK_KIND_VSYNC */
      };
      wlr_presentation_feedback_send_presented(pending->feedback, &event);
      if (interval > 0 && self->last_reported_interval != interval) {
        self->last_reported_interval = interval;
        g_message("Brave presentation interval from GTK: %" G_GINT64_FORMAT " us (%.2f Hz)", interval, 1000000.0 / interval);
      }
    }
    wlr_presentation_feedback_destroy(pending->feedback);
    g_free(g_queue_pop_head(&self->presentations));
  }
}
static void discard_presentations(GnosisCompositorView *self) {
  PresentedFrame *pending;
  while ((pending = g_queue_pop_head(&self->presentations))) {
    wlr_presentation_feedback_destroy(pending->feedback); g_free(pending);
  }
}
static void sync_output_refresh(GnosisCompositor *server) {
  int refresh = 0;
  // A process currently shares one nested output. Use the fastest monitor of
  // its mapped content views; each view's callbacks still follow its GTK clock.
  for (guint i = 0; i < server->hosts->len; i++) {
    GtkWidget *host = g_ptr_array_index(server->hosts, i);
    if (!gtk_widget_get_mapped(host)) continue;
    GtkNative *native = gtk_widget_get_native(host);
    GdkSurface *surface = native ? gtk_native_get_surface(native) : NULL;
    GdkMonitor *monitor = surface ? gdk_display_get_monitor_at_surface(gtk_widget_get_display(host), surface) : NULL;
    if (monitor) refresh = MAX(refresh, gdk_monitor_get_refresh_rate(monitor));
  }
  if (refresh <= 0 || refresh == server->output->refresh) return;
  struct wlr_output_state state; wlr_output_state_init(&state);
  wlr_output_state_set_custom_mode(&state, server->output->width, server->output->height, refresh);
  gboolean committed = wlr_output_commit_state(server->output, &state);
  wlr_output_state_finish(&state);
  if (committed) g_message("Nested output refresh follows GTK monitor: %.3f Hz", refresh / 1000.0);
}
static gboolean tick(GtkWidget *widget, GdkFrameClock *clock, gpointer data) {
  (void)data;
  GnosisCompositorView *self = GNOSIS_COMPOSITOR_VIEW(widget);
  if (self->server->display && self->active) {
    /* Allocation/map can happen before the native xdg surface is initialized.
     * Reconcile from GTK's settled allocation as well, so that skipped or
     * coalesced startup resizes cannot leave content at the previous size.
     * configure() only sends a protocol update when the dimensions changed. */
    if (self->active->toplevel->base->initialized) configure(self->active);
    finish_presentations(self, clock);
    sync_output_refresh(self->server);
    gint64 usec = gdk_frame_clock_get_frame_time(clock);
    if (g_getenv("GNOSIS_TRACE_FRAMES")) {
      if (!self->trace_start) self->trace_start = usec;
      else self->trace_ticks++;
      if (usec - self->trace_start >= 5 * G_USEC_PER_SEC) {
        GtkNative *native = gtk_widget_get_native(widget);
        GdkSurface *surface = native ? gtk_native_get_surface(native) : NULL;
        GdkMonitor *monitor = surface ? gdk_display_get_monitor_at_surface(gtk_widget_get_display(widget), surface) : NULL;
        g_message("GTK frame clock: %.2f callbacks/s, monitor=%s, backend=%s", self->trace_ticks * 1000000.0 / (usec-self->trace_start),
          monitor ? gdk_monitor_get_connector(monitor) : "unknown", G_OBJECT_TYPE_NAME(gtk_widget_get_display(widget)));
        self->trace_start = usec; self->trace_ticks = 0;
      }
    }
    struct timespec now = {usec / 1000000, (usec % 1000000) * 1000};
    wlr_xdg_surface_for_each_surface(self->active->toplevel->base, frame_surface, &now);
    if (self->server->cursor_surface && pointer_host(self->server) == self)
      wlr_surface_send_frame_done(self->server->cursor_surface, &now);
    wl_display_flush_clients(self->server->display);
  }
  return G_SOURCE_CONTINUE;
}
static void surface_commit(struct wl_listener *listener, void *data) {
  (void)data; Surface *s = wl_container_of(listener, s, commit);
  if (s->surface == s->server->cursor_surface) update_cursor(s->server);
  struct wlr_surface *root = wlr_surface_get_root_surface(s->surface);
  /* A subsurface or native popup belongs to its parent tab. Invalidating every
   * host made one page's animation invalidate all overview thumbnails too. */
  for (guint depth = 0; depth < 64; depth++) {
    struct wlr_xdg_surface *xdg = wlr_xdg_surface_try_from_wlr_surface(root);
    if (!xdg || xdg->role != WLR_XDG_SURFACE_ROLE_POPUP || !xdg->popup->parent) break;
    root = wlr_surface_get_root_surface(xdg->popup->parent);
  }
  for (guint i = 0; i < s->server->hosts->len; i++) {
    GnosisCompositorView *host = g_ptr_array_index(s->server->hosts, i);
    if (host->active && host->active->toplevel->base->surface == root)
      gtk_widget_queue_draw(GTK_WIDGET(host));
  }
}
static void surface_destroy(struct wl_listener *listener, void *data) {
  (void)data; Surface *s = wl_container_of(listener, s, destroy);
  if (s->surface == s->server->cursor_surface) s->server->cursor_surface = NULL;
  wl_list_remove(&s->commit.link); wl_list_remove(&s->destroy.link); g_free(s);
}

static GnosisCompositorView *pointer_host(GnosisCompositor *server) {
  struct wlr_surface *surface = server->seat->pointer_state.focused_surface;
  if (!surface) return NULL;
  struct wlr_surface *root = wlr_surface_get_root_surface(surface);
  for (guint depth = 0; depth < 64; depth++) {
    struct wlr_xdg_surface *xdg = wlr_xdg_surface_try_from_wlr_surface(root);
    if (!xdg || xdg->role != WLR_XDG_SURFACE_ROLE_POPUP || !xdg->popup->parent) break;
    root = wlr_surface_get_root_surface(xdg->popup->parent);
  }
  for (guint i = 0; i < server->hosts->len; i++) {
    GnosisCompositorView *host = g_ptr_array_index(server->hosts, i);
    if (host->active && host->active->toplevel->base->surface == root) return host;
  }
  return NULL;
}

typedef struct { GdkTexture *texture; int width, height, x, y; } CursorImage;
static GdkTexture *cursor_texture(GdkCursor *cursor, int size, double scale,
                                  int *width, int *height, int *x, int *y, gpointer data) {
  (void)cursor; (void)size; (void)scale;
  CursorImage *image = data;
  *width = image->width; *height = image->height; *x = image->x; *y = image->y;
  return g_object_ref(image->texture);
}
static void free_cursor_image(gpointer data) {
  CursorImage *image = data; g_object_unref(image->texture); g_free(image);
}
static void update_cursor(GnosisCompositor *server) {
  GnosisCompositorView *host = pointer_host(server);
  struct wlr_surface *surface = server->cursor_surface;
  if (!host || host->captured || !surface) return;
  int width = surface->current.width, height = surface->current.height;
  if (width <= 0 || height <= 0 || width > 512 || height > 512) return;
  GdkTexture *texture = surface_texture(host, surface);
  if (!texture) return;
  CursorImage *image = g_new0(CursorImage, 1);
  *image = (CursorImage){texture, width, height,
    CLAMP(server->cursor_hotspot_x, 0, width - 1), CLAMP(server->cursor_hotspot_y, 0, height - 1)};
  GdkCursor *cursor = gdk_cursor_new_from_callback(cursor_texture, image, free_cursor_image, NULL);
  gtk_widget_set_cursor(GTK_WIDGET(host), cursor); g_object_unref(cursor);
}
static gboolean cursor_request_allowed(GnosisCompositor *server, struct wlr_seat_client *client, uint32_t serial) {
  return client == server->seat->pointer_state.focused_client &&
    wlr_seat_client_validate_event_serial(client, serial);
}
static void request_cursor(struct wl_listener *listener, void *data) {
  GnosisCompositor *server = wl_container_of(listener, server, request_cursor);
  struct wlr_seat_pointer_request_set_cursor_event *event = data;
  if (!cursor_request_allowed(server, event->seat_client, event->serial)) return;
  server->cursor_surface = event->surface;
  server->cursor_hotspot_x = event->hotspot_x; server->cursor_hotspot_y = event->hotspot_y;
  GnosisCompositorView *host = pointer_host(server);
  if (!event->surface && host && !host->captured) gtk_widget_set_cursor_from_name(GTK_WIDGET(host), "none");
  else update_cursor(server);
}
static void request_cursor_shape(struct wl_listener *listener, void *data) {
  GnosisCompositor *server = wl_container_of(listener, server, request_cursor_shape);
  struct wlr_cursor_shape_manager_v1_request_set_shape_event *event = data;
  if (event->device_type != WLR_CURSOR_SHAPE_MANAGER_V1_DEVICE_TYPE_POINTER ||
      !cursor_request_allowed(server, event->seat_client, event->serial)) return;
  server->cursor_surface = NULL;
  GnosisCompositorView *host = pointer_host(server);
  if (host && !host->captured)
    gtk_widget_set_cursor_from_name(GTK_WIDGET(host), wlr_cursor_shape_v1_name(event->shape));
}
static void new_surface(struct wl_listener *listener, void *data) {
  GnosisCompositor *server = wl_container_of(listener, server, new_surface);
  struct wlr_surface *surface = data; Surface *s = g_new0(Surface, 1); s->server = server; s->surface = surface;
  listen(&surface->events.commit, &s->commit, surface_commit);
  listen(&surface->events.destroy, &s->destroy, surface_destroy);
}
static void bind_view(View *view) {
  if (view->host) return;
  for (guint i = 0; i < view->server->hosts->len; i++) {
    GnosisCompositorView *host = g_ptr_array_index(view->server->hosts, i);
    if (host->active) continue;
    if (g_strcmp0(host->token, "__gnosis_devtools__") == 0) {
      g_autofree char *app = g_ascii_strdown(view->toplevel->app_id ? view->toplevel->app_id : "", -1);
      if (!strstr(app, "devtools") && !(view->toplevel->title && strstr(view->toplevel->title, "DevTools"))) continue;
      view->host = host; host->active = view; return;
    }
    if (host->token && !(view->toplevel->app_id && strstr(view->toplevel->app_id, host->token)) &&
        !(view->toplevel->title && strstr(view->toplevel->title, host->token))) continue;
    view->host = host; host->active = view; return;
  }
}
static void configure_size(View *view, int width, int height) {
  if (width <= 0 || height <= 0) return;
  if (width != view->requested_width || height != view->requested_height) {
    view->requested_width = width; view->requested_height = height;
    wlr_xdg_toplevel_set_size(view->toplevel, width, height);
  }
}
static void configure(View *view) {
  bind_view(view);
  GtkWidget *widget = view->host ? GTK_WIDGET(view->host) : NULL;
  // Hidden restore tabs have no allocation yet. Keep their last positive size
  // until GTK maps them, using the original fallback for newly created tabs.
  int width = widget && gtk_widget_get_mapped(widget) ? MAX(1,gtk_widget_get_width(widget)) : (view->requested_width > 0 ? view->requested_width : 1280);
  int height = widget && gtk_widget_get_mapped(widget) ? MAX(1,gtk_widget_get_height(widget)) : (view->requested_height > 0 ? view->requested_height : 800);
  configure_size(view, width, height);
}
static void size_allocate(GtkWidget *widget, int width, int height, int baseline) {
  (void)baseline;
  GnosisCompositorView *self = GNOSIS_COMPOSITOR_VIEW(widget);
  if (self->active && self->active->toplevel->base->initialized) {
    if (gtk_widget_get_mapped(widget)) configure_size(self->active, width, height);
    else configure(self->active);
  }
  for (GtkWidget *child = gtk_widget_get_first_child(widget); child; child = gtk_widget_get_next_sibling(child))
    if (GTK_IS_POPOVER(child) && gtk_widget_get_visible(child)) gtk_popover_present(GTK_POPOVER(child));
}
static void mapped(GtkWidget *widget, gpointer data) {
  (void)data; GnosisCompositorView *self = GNOSIS_COMPOSITOR_VIEW(widget);
  if (self->active && self->active->toplevel->base->initialized) configure(self->active);
  gtk_widget_queue_allocate(widget);
}
static void view_commit(struct wl_listener *listener, void *data) {
  (void)data; View *view = wl_container_of(listener, view, commit);
  if (view->toplevel->base->initial_commit) {
    if (view->decoration) wlr_xdg_toplevel_decoration_v1_set_mode(view->decoration, WLR_XDG_TOPLEVEL_DECORATION_V1_MODE_SERVER_SIDE);
    configure(view); wlr_xdg_toplevel_set_activated(view->toplevel, TRUE);
  }
}
static void view_map(struct wl_listener *listener, void *data) {
  (void)data; View *view = wl_container_of(listener, view, map);
  bind_view(view);
  if (!view->host) {
    g_warning("Wayland surface has no registered GTK tab: %s", view->toplevel->app_id ? view->toplevel->app_id : "(unnamed)");
    return;
  }
  wlr_surface_send_enter(view->toplevel->base->surface, view->server->output);
  configure(view);
  gtk_widget_queue_allocate(GTK_WIDGET(view->host));
  gtk_widget_queue_draw(GTK_WIDGET(view->host));
  if (gtk_widget_has_focus(GTK_WIDGET(view->host))) focus_entered(NULL, view->host);
  g_signal_emit_by_name(view->host, "surface-ready");
  g_message("Wayland toplevel mapped inside GTK tab: %s", view->toplevel->app_id ? view->toplevel->app_id : "(unnamed)");
}
static void view_unmap(struct wl_listener *listener, void *data) {
  (void)data; View *view = wl_container_of(listener, view, unmap);
  restore_fullscreen(view);
  if (view->host) {
    cancel_touches(view->host);
    stop_capture(view->host); gtk_widget_queue_draw(GTK_WIDGET(view->host));
  }
}
static void view_destroy(struct wl_listener *listener, void *data) {
  (void)data; View *v = wl_container_of(listener, v, destroy);
  restore_fullscreen(v); g_weak_ref_clear(&v->fullscreen_window);
  if (v->host && v->host->active == v) { cancel_touches(v->host); stop_capture(v->host); v->host->active = NULL; }
  wl_list_remove(&v->link); wl_list_remove(&v->commit.link); wl_list_remove(&v->map.link);
  wl_list_remove(&v->unmap.link); wl_list_remove(&v->destroy.link);
  wl_list_remove(&v->maximize.link); wl_list_remove(&v->fullscreen.link);
  wl_list_remove(&v->decoration_destroy.link); g_free(v);
}
static void maximize(struct wl_listener *listener, void *data) {
  (void)data; View *v = wl_container_of(listener, v, maximize);
  wlr_xdg_toplevel_set_maximized(v->toplevel, TRUE); configure(v);
}
static void fullscreen(struct wl_listener *listener, void *data) {
  (void)data; View *v = wl_container_of(listener, v, fullscreen);
  gboolean requested = v->toplevel->requested.fullscreen;
  GtkRoot *root = v->host ? gtk_widget_get_root(GTK_WIDGET(v->host)) : NULL;
  if (requested && root && GTK_IS_WINDOW(root) && gtk_widget_get_mapped(GTK_WIDGET(v->host))) {
    if (!v->content_fullscreen) {
      v->content_fullscreen = TRUE;
      g_signal_emit_by_name(v->host, "fullscreen-changed", TRUE);
    }
    if (!gtk_window_is_fullscreen(GTK_WINDOW(root))) {
      v->owns_fullscreen = TRUE; g_weak_ref_set(&v->fullscreen_window, root);
      gtk_window_fullscreen(GTK_WINDOW(root));
    }
  } else if (requested) {
    requested = FALSE;
  } else {
    restore_fullscreen(v);
  }
  wlr_xdg_toplevel_set_fullscreen(v->toplevel, requested); configure(v);
}
static void restore_fullscreen(View *view) {
  if (view->content_fullscreen) {
    view->content_fullscreen = FALSE;
    if (view->host) g_signal_emit_by_name(view->host, "fullscreen-changed", FALSE);
  }
  if (!view->owns_fullscreen) return;
  view->owns_fullscreen = FALSE;
  GtkWindow *window = g_weak_ref_get(&view->fullscreen_window);
  g_weak_ref_set(&view->fullscreen_window, NULL);
  if (window) { gtk_window_unfullscreen(window); g_object_unref(window); }
}
static void new_toplevel(struct wl_listener *listener, void *data) {
  GnosisCompositor *server = wl_container_of(listener, server, new_toplevel);
  struct wlr_xdg_toplevel *toplevel = data; View *v = g_new0(View, 1);
  v->server = server; v->toplevel = toplevel; wl_list_insert(&server->views, &v->link);
  g_weak_ref_init(&v->fullscreen_window, NULL);
  toplevel->base->data = v;
  wl_list_init(&v->decoration_destroy.link);
  listen(&toplevel->base->surface->events.commit, &v->commit, view_commit);
  listen(&toplevel->base->surface->events.map, &v->map, view_map);
  listen(&toplevel->base->surface->events.unmap, &v->unmap, view_unmap);
  listen(&toplevel->events.destroy, &v->destroy, view_destroy);
  listen(&toplevel->events.request_maximize, &v->maximize, maximize);
  listen(&toplevel->events.request_fullscreen, &v->fullscreen, fullscreen);
}
static View *popup_view(Popup *p) {
  struct wlr_surface *parent = p->popup->parent;
  for (guint depth = 0; parent && depth < 64; depth++) {
    struct wlr_xdg_surface *xdg = wlr_xdg_surface_try_from_wlr_surface(wlr_surface_get_root_surface(parent));
    if (!xdg) return NULL;
    if (xdg->role == WLR_XDG_SURFACE_ROLE_TOPLEVEL) return xdg->data;
    if (xdg->role != WLR_XDG_SURFACE_ROLE_POPUP) return NULL;
    parent = xdg->popup->parent;
  }
  return NULL;
}
static void configure_popup(Popup *p) {
  View *v = popup_view(p);
  if (v) {
    struct wlr_box bounds = v->toplevel->base->geometry;
    wlr_xdg_popup_unconstrain_from_box(p->popup, &bounds);
  }
  wlr_xdg_surface_schedule_configure(p->popup->base);
}
static void popup_commit(struct wl_listener *listener, void *data) {
  (void)data; Popup *p = wl_container_of(listener, p, commit);
  if (p->popup->base->initial_commit) configure_popup(p);
}
static void popup_reposition(struct wl_listener *listener, void *data) {
  (void)data; Popup *p = wl_container_of(listener, p, reposition);
  configure_popup(p);
}
static void popup_unmap(struct wl_listener *listener, void *data) {
  (void)data; Popup *p = wl_container_of(listener, p, unmap);
  View *v = popup_view(p);
  if (v && v->host) gtk_widget_queue_draw(GTK_WIDGET(v->host));
}
static void popup_destroy(struct wl_listener *listener, void *data) {
  (void)data; Popup *p = wl_container_of(listener, p, destroy);
  wl_list_remove(&p->commit.link); wl_list_remove(&p->reposition.link);
  wl_list_remove(&p->unmap.link); wl_list_remove(&p->destroy.link); g_free(p);
}
static void new_popup(struct wl_listener *listener, void *data) {
  (void)listener; Popup *p = g_new0(Popup, 1); p->popup = data;
  listen(&p->popup->base->surface->events.commit, &p->commit, popup_commit);
  listen(&p->popup->events.reposition, &p->reposition, popup_reposition);
  listen(&p->popup->base->surface->events.unmap, &p->unmap, popup_unmap);
  listen(&p->popup->events.destroy, &p->destroy, popup_destroy);
}
static void decoration_destroy(struct wl_listener *listener, void *data) {
  (void)data; View *v = wl_container_of(listener, v, decoration_destroy);
  v->decoration = NULL; wl_list_remove(&v->decoration_destroy.link); wl_list_init(&v->decoration_destroy.link);
}
static void new_decoration(struct wl_listener *listener, void *data) {
  (void)listener;
  struct wlr_xdg_toplevel_decoration_v1 *decoration = data;
  View *view = decoration->toplevel->base->data;
  view->decoration = decoration;
  listen(&decoration->events.destroy, &view->decoration_destroy, decoration_destroy);
  if (decoration->toplevel->base->initialized)
    wlr_xdg_toplevel_decoration_v1_set_mode(decoration, WLR_XDG_TOPLEVEL_DECORATION_V1_MODE_SERVER_SIDE);
}
#include "touch-input.inc"
static void motion(GtkEventControllerMotion *controller, double x, double y, GnosisCompositorView *self) {
  if (!self->active || self->captured) return;
  struct wlr_xdg_surface *xdg = self->active->toplevel->base;
  view_coordinates(self, &x, &y);
  double sx, sy; struct wlr_surface *surface = wlr_xdg_surface_surface_at(xdg, x, y, &sx, &sy);
  if (!surface) { wlr_seat_pointer_notify_clear_focus(self->server->seat); return; }
  guint32 time = gtk_event_controller_get_current_event_time(GTK_EVENT_CONTROLLER(controller));
  wlr_seat_pointer_notify_enter(self->server->seat, surface, sx, sy);
  wlr_seat_pointer_notify_motion(self->server->seat, time, sx, sy); wlr_seat_pointer_notify_frame(self->server->seat);
  try_capture(self);
}
static void leave(GtkEventControllerMotion *controller, GnosisCompositorView *self) {
  (void)controller; if (self->server->seat && !self->captured) wlr_seat_pointer_notify_clear_focus(self->server->seat);
}
static void button(GtkGestureClick *gesture, int count, double x, double y, GnosisCompositorView *self) {
  (void)count; if (!self->active) return;
  guint n = gtk_gesture_single_get_current_button(GTK_GESTURE_SINGLE(gesture));
  if (n > 3) return; // Epiphany handles the back/forward mouse buttons.
  GdkEvent *event = gtk_event_controller_get_current_event(GTK_EVENT_CONTROLLER(gesture));
  if (!event) return;
  g_set_object(&self->pointer_device, gdk_event_get_device(event));
  const gboolean pressed = gdk_event_get_event_type(event) == GDK_BUTTON_PRESS;
  const guint bit = 1u << (n ? n : 1);
  if (!pressed && !(self->pressed_buttons & bit)) return;
  if (pressed) self->pressed_buttons |= bit; else self->pressed_buttons &= ~bit;
  if (pressed) {
    // The web surface owns drags that start here. Ancestor pan gestures must
    // not take the sequence away halfway through selecting webpage text.
    gtk_gesture_set_state(GTK_GESTURE(gesture),GTK_EVENT_SEQUENCE_CLAIMED);
    gtk_widget_grab_focus(GTK_WIDGET(self));
    wlr_seat_keyboard_notify_enter(self->server->seat, self->active->toplevel->base->surface,
      self->server->keyboard.keycodes, self->server->keyboard.num_keycodes, &self->server->keyboard.modifiers);
  }
  sync_modifiers(GTK_EVENT_CONTROLLER(gesture), self);
  uint32_t code = n <= 1 ? BTN_LEFT : n == 2 ? BTN_MIDDLE : BTN_RIGHT;
  if (n == 3 && gdk_event_get_event_type(event) == GDK_BUTTON_PRESS) {
    self->context_x = x; self->context_y = y; self->context_time = g_get_monotonic_time();
  }
  if (pressed) self->capture_inhibited = FALSE;
  wlr_seat_pointer_notify_button(self->server->seat, gdk_event_get_time(event), code,
    pressed ? WL_POINTER_BUTTON_STATE_PRESSED : WL_POINTER_BUTTON_STATE_RELEASED);
  wlr_seat_pointer_notify_frame(self->server->seat);
}
static gboolean scroll(GtkEventControllerScroll *controller, double dx, double dy, GnosisCompositorView *self) {
  if (!self->server->seat) return FALSE;
  sync_modifiers(GTK_EVENT_CONTROLLER(controller), self);
  if (gtk_event_controller_get_current_event_state(GTK_EVENT_CONTROLLER(controller)) & GDK_CONTROL_MASK) {
    if (dy) gtk_widget_activate_action(GTK_WIDGET(self),dy < 0 ? "win.zoom-in" : "win.zoom-out",NULL);
    return TRUE;
  }
  guint32 time = gtk_event_controller_get_current_event_time(GTK_EVENT_CONTROLLER(controller));
  gboolean finger = gtk_event_controller_scroll_get_unit(controller) == GDK_SCROLL_UNIT_SURFACE;
  if (dy) {
    wlr_seat_pointer_notify_axis(self->server->seat, time, WL_POINTER_AXIS_VERTICAL_SCROLL, finger ? dy : dy * 15, finger ? 0 : dy * 120,
      finger ? WL_POINTER_AXIS_SOURCE_FINGER : WL_POINTER_AXIS_SOURCE_WHEEL, WL_POINTER_AXIS_RELATIVE_DIRECTION_IDENTICAL);
    if (finger) self->scroll_axes |= 1;
  }
  if (dx) {
    wlr_seat_pointer_notify_axis(self->server->seat, time, WL_POINTER_AXIS_HORIZONTAL_SCROLL, finger ? dx : dx * 15, finger ? 0 : dx * 120,
      finger ? WL_POINTER_AXIS_SOURCE_FINGER : WL_POINTER_AXIS_SOURCE_WHEEL, WL_POINTER_AXIS_RELATIVE_DIRECTION_IDENTICAL);
    if (finger) self->scroll_axes |= 2;
  }
  wlr_seat_pointer_notify_frame(self->server->seat); return TRUE;
}
static void scroll_end(GtkEventControllerScroll *controller, GnosisCompositorView *self) {
  if (!self->server || !self->server->seat) return;
  if (pointer_host(self->server) != self) { self->scroll_axes = 0; return; }
  guint32 time = gtk_event_controller_get_current_event_time(GTK_EVENT_CONTROLLER(controller));
  if (self->scroll_axes & 1) wlr_seat_pointer_notify_axis(self->server->seat, time, WL_POINTER_AXIS_VERTICAL_SCROLL, 0, 0,
    WL_POINTER_AXIS_SOURCE_FINGER, WL_POINTER_AXIS_RELATIVE_DIRECTION_IDENTICAL);
  if (self->scroll_axes & 2) wlr_seat_pointer_notify_axis(self->server->seat, time, WL_POINTER_AXIS_HORIZONTAL_SCROLL, 0, 0,
    WL_POINTER_AXIS_SOURCE_FINGER, WL_POINTER_AXIS_RELATIVE_DIRECTION_IDENTICAL);
  if (self->scroll_axes) wlr_seat_pointer_notify_frame(self->server->seat);
  self->scroll_axes = 0;
}
void gnosis_compositor_view_release_buttons(GnosisCompositorView *self) {
  if (self->server && self->server->export_drag) return;
  if (!self->server || !self->server->seat) { self->pressed_buttons = 0; return; }
  const guint32 time = (guint32)(g_get_monotonic_time() / 1000);
  for (guint n = 1; n <= 3; ++n) if (self->pressed_buttons & (1u << n))
    wlr_seat_pointer_notify_button(self->server->seat,time,n == 1 ? BTN_LEFT : n == 2 ? BTN_MIDDLE : BTN_RIGHT,WL_POINTER_BUTTON_STATE_RELEASED);
  if (self->pressed_buttons) wlr_seat_pointer_notify_frame(self->server->seat);
  self->pressed_buttons = 0;
}
static void key(GtkEventControllerKey *controller, guint code, gboolean pressed, GnosisCompositorView *self) {
  if (!self->active || code < 8) return;
  struct wlr_keyboard_key_event event = {.time_msec = gtk_event_controller_get_current_event_time(GTK_EVENT_CONTROLLER(controller)),
    .keycode = code - 8, .update_state = TRUE, .state = pressed ? WL_KEYBOARD_KEY_STATE_PRESSED : WL_KEYBOARD_KEY_STATE_RELEASED};
  wlr_keyboard_notify_key(&self->server->keyboard, &event);
  wlr_seat_keyboard_notify_modifiers(self->server->seat, &self->server->keyboard.modifiers);
  wlr_seat_keyboard_notify_key(self->server->seat, event.time_msec, event.keycode, event.state);
}
const char *gnosis_compositor_vim_key(guint value, GdkModifierType modifiers) {
  if (!(modifiers & GDK_CONTROL_MASK) || (modifiers & (GDK_ALT_MASK | GDK_SUPER_MASK))) return NULL;
  switch (gdk_keyval_to_lower(value)) {
    case GDK_KEY_r: return "redo";
    case GDK_KEY_d: return "scroll-down";
    case GDK_KEY_u: return "scroll-up";
    case GDK_KEY_w: return "delete-word";
    case GDK_KEY_h: return "backspace";
    default: return NULL;
  }
}
void gnosis_compositor_view_set_vim_field(GnosisCompositorView *self, gboolean enabled) { self->vim_field = enabled; }
gboolean gnosis_compositor_is_host_shortcut(guint value, GdkModifierType modifiers) {
  value = gdk_keyval_to_lower(value);
  if (value == GDK_KEY_F3) return TRUE;
  if (modifiers & GDK_SUPER_MASK) return TRUE;
  if ((modifiers & GDK_ALT_MASK) && (value == GDK_KEY_Left || value == GDK_KEY_Right || value == GDK_KEY_Home)) return TRUE;
  if (!(modifiers & GDK_CONTROL_MASK)) return FALSE;
  switch (value) {
    case GDK_KEY_c: case GDK_KEY_x: case GDK_KEY_v: case GDK_KEY_a:
    case GDK_KEY_z: case GDK_KEY_y:
    case GDK_KEY_t: case GDK_KEY_n: case GDK_KEY_w: case GDK_KEY_l:
    case GDK_KEY_f: case GDK_KEY_g: case GDK_KEY_r: case GDK_KEY_s: case GDK_KEY_p:
    case GDK_KEY_h: case GDK_KEY_d: case GDK_KEY_o: case GDK_KEY_q:
    case GDK_KEY_Tab: case GDK_KEY_ISO_Left_Tab: case GDK_KEY_Page_Up: case GDK_KEY_Page_Down:
    case GDK_KEY_plus: case GDK_KEY_minus: case GDK_KEY_equal: case GDK_KEY_0:
    case GDK_KEY_1: case GDK_KEY_2: case GDK_KEY_3: case GDK_KEY_4: case GDK_KEY_5:
    case GDK_KEY_6: case GDK_KEY_7: case GDK_KEY_8: case GDK_KEY_9:
      return TRUE;
    default: return FALSE;
  }
}
static gboolean key_pressed(GtkEventControllerKey *controller, guint value, guint code, GdkModifierType modifiers, GnosisCompositorView *self) {
  const char *vim = self->vim_field ? gnosis_compositor_vim_key(value, modifiers) : NULL;
  if (vim) { g_signal_emit_by_name(self, "vim-command", vim); return TRUE; }
  if (value == GDK_KEY_Escape && self->constraint) {
    self->capture_inhibited = TRUE; stop_capture(self);
    // Let Chromium also leave HTML fullscreen when Escape releases capture.
    key(controller, code, TRUE, self);
    return TRUE;
  }
  if (value != GDK_KEY_Escape) self->capture_inhibited = FALSE;
  if (gnosis_compositor_is_host_shortcut(value, modifiers)) return FALSE;
  key(controller, code, TRUE, self);
  return !(modifiers & (GDK_CONTROL_MASK | GDK_ALT_MASK | GDK_SUPER_MASK));
}
static void key_released(GtkEventControllerKey *controller, guint value, guint code, GdkModifierType modifiers, GnosisCompositorView *self) {
  if (self->vim_field && gnosis_compositor_vim_key(value, modifiers)) return;
  if (!gnosis_compositor_is_host_shortcut(value, modifiers)) key(controller, code, FALSE, self);
}
#include "drag-bridge.inc"
static void server_dispose(GObject *object) {
  GnosisCompositor *self = GNOSIS_COMPOSITOR(object);
  if (self->display && !self->shutting_down) {
    self->shutting_down = TRUE;
    if (self->touch_frame) { g_source_remove(self->touch_frame); self->touch_frame = 0; }
    gnosis_clipboard_bridge_free(self->clipboard); self->clipboard = NULL;
    if (self->dispatch) { g_source_remove(self->dispatch); self->dispatch = 0; }
    // These listeners belong to this server, not to the wlroots globals.
    // wlroots requires its public event lists to be empty during destruction.
    struct wl_listener *listeners[] = {&self->new_surface, &self->new_toplevel, &self->new_popup, &self->new_constraint, &self->decoration, &self->request_drag, &self->request_cursor, &self->request_cursor_shape};
    for (guint i = 0; i < G_N_ELEMENTS(listeners); ++i) {
      if (listeners[i]->link.next) wl_list_remove(&listeners[i]->link);
      wl_list_init(&listeners[i]->link);
    }
    wl_display_destroy_clients(self->display);
    if (self->keyboard_ready) wlr_keyboard_finish(&self->keyboard);
    if (self->backend) { wlr_backend_destroy(self->backend); self->backend = NULL; }
    wl_display_destroy(self->display); self->display = NULL;
    if (self->renderer) { wlr_renderer_destroy(self->renderer); self->renderer = NULL; }
    if (self->drm_fd >= 0) { close(self->drm_fd); self->drm_fd = -1; }
    if (self->socket) unlink(self->socket);
    if (self->directory) rmdir(self->directory);
    g_clear_pointer(&self->socket, g_free); g_clear_pointer(&self->directory, g_free);
  }
  g_clear_pointer(&self->hosts, g_ptr_array_unref);
  G_OBJECT_CLASS(gnosis_compositor_parent_class)->dispose(object);
}
static void gnosis_compositor_class_init(GnosisCompositorClass *klass) { G_OBJECT_CLASS(klass)->dispose = server_dispose; }
static void gnosis_compositor_init(GnosisCompositor *self) {
  self->drm_fd = -1; wl_list_init(&self->views); self->hosts = g_ptr_array_new();
}
static void dispose(GObject *object) {
  GnosisCompositorView *self = GNOSIS_COMPOSITOR_VIEW(object);
  cancel_swipe(self);
  cancel_touches(self);
  cancel_view_drag(self);
  g_clear_object(&self->pointer_device);
  stop_capture(self); gnosis_outer_pointer_free(self->outer_pointer); self->outer_pointer = NULL;
  discard_presentations(self);
  if (self->server) {
    View *view;
    wl_list_for_each(view, &self->server->views, link) if (view->host == self) view->host = NULL;
    self->active = NULL;
    g_ptr_array_remove(self->server->hosts, self);
    g_clear_object(&self->server);
  }
  g_clear_pointer(&self->token, g_free);
  g_clear_pointer(&self->touches, g_hash_table_unref);
  G_OBJECT_CLASS(gnosis_compositor_view_parent_class)->dispose(object);
}
static void gnosis_compositor_view_class_init(GnosisCompositorViewClass *klass) {
  G_OBJECT_CLASS(klass)->dispose = dispose;
  GtkWidgetClass *widget = GTK_WIDGET_CLASS(klass);
  widget->snapshot = snapshot; widget->size_allocate = size_allocate;
  gtk_widget_class_set_css_name(widget, "gnosis-compositor-view");
  g_signal_new("surface-ready", G_TYPE_FROM_CLASS(klass), G_SIGNAL_RUN_LAST, 0, NULL, NULL, NULL, G_TYPE_NONE, 0);
  g_signal_new("touch-navigation", G_TYPE_FROM_CLASS(klass), G_SIGNAL_RUN_LAST, 0, NULL, NULL, NULL, G_TYPE_NONE, 1, G_TYPE_INT);
  g_signal_new("touch-swipe-begin", G_TYPE_FROM_CLASS(klass), G_SIGNAL_RUN_LAST, 0, g_signal_accumulator_true_handled, NULL, NULL, G_TYPE_BOOLEAN, 1, G_TYPE_INT);
  g_signal_new("vim-command", G_TYPE_FROM_CLASS(klass), G_SIGNAL_RUN_LAST, 0, NULL, NULL, NULL, G_TYPE_NONE, 1, G_TYPE_STRING);
  g_signal_new("fullscreen-changed", G_TYPE_FROM_CLASS(klass), G_SIGNAL_RUN_LAST, 0, NULL, NULL, NULL, G_TYPE_NONE, 1, G_TYPE_BOOLEAN);
  g_signal_new("drag-preview-image", G_TYPE_FROM_CLASS(klass), G_SIGNAL_RUN_LAST, 0, NULL, NULL, NULL, G_TYPE_NONE, 2, GDK_TYPE_DRAG, G_TYPE_STRING);
}
static void gnosis_compositor_view_init(GnosisCompositorView *self) {
  self->touches = g_hash_table_new_full(g_direct_hash, g_direct_equal, NULL, g_free);
  wl_list_init(&self->constraint_destroy.link);
  gtk_widget_set_focusable(GTK_WIDGET(self), TRUE); gtk_widget_set_hexpand(GTK_WIDGET(self), TRUE);
  gtk_widget_set_vexpand(GTK_WIDGET(self), TRUE); gtk_widget_set_overflow(GTK_WIDGET(self), GTK_OVERFLOW_HIDDEN);
  setup_drop_target(self);
  GtkEventController *controller = gtk_event_controller_legacy_new();
  gtk_event_controller_set_propagation_phase(controller, GTK_PHASE_CAPTURE);
  g_signal_connect(controller, "event", G_CALLBACK(touch_event), self);
  gtk_widget_add_controller(GTK_WIDGET(self), controller);
  controller = gtk_event_controller_motion_new();
  g_signal_connect(controller, "motion", G_CALLBACK(motion), self); g_signal_connect(controller, "enter", G_CALLBACK(motion), self);
  g_signal_connect(controller, "leave", G_CALLBACK(leave), self); gtk_widget_add_controller(GTK_WIDGET(self), controller);
  GtkGesture *click = gtk_gesture_click_new(); gtk_gesture_single_set_button(GTK_GESTURE_SINGLE(click), 0);
  g_signal_connect(click, "pressed", G_CALLBACK(button), self); g_signal_connect(click, "released", G_CALLBACK(button), self);
  g_signal_connect_swapped(click,"cancel",G_CALLBACK(gnosis_compositor_view_release_buttons),self);
  gtk_widget_add_controller(GTK_WIDGET(self), GTK_EVENT_CONTROLLER(click));
  controller = gtk_event_controller_scroll_new(GTK_EVENT_CONTROLLER_SCROLL_BOTH_AXES);
  g_signal_connect(controller, "scroll-end", G_CALLBACK(scroll_end), self);
  g_signal_connect(controller, "scroll", G_CALLBACK(scroll), self); gtk_widget_add_controller(GTK_WIDGET(self), controller);
  controller = gtk_event_controller_key_new(); gtk_event_controller_set_propagation_phase(controller, GTK_PHASE_BUBBLE);
  g_signal_connect(controller, "key-pressed", G_CALLBACK(key_pressed), self); g_signal_connect(controller, "key-released", G_CALLBACK(key_released), self);
  gtk_widget_add_controller(GTK_WIDGET(self), controller);
  controller = gtk_event_controller_focus_new();
  g_signal_connect(controller, "enter", G_CALLBACK(focus_entered), self);
  g_signal_connect(controller, "leave", G_CALLBACK(focus_left), self); gtk_widget_add_controller(GTK_WIDGET(self), controller);
  g_signal_connect(self, "unmap", G_CALLBACK(unmapped), self);
  g_signal_connect(self, "map", G_CALLBACK(mapped), NULL);
  gtk_widget_add_tick_callback(GTK_WIDGET(self), tick, NULL, NULL);
}
static gboolean clipboard_allowed(gpointer data) {
  GnosisCompositor *self = data;
  for (guint i = 0; i < self->hosts->len; i++) {
    GtkWidget *widget = g_ptr_array_index(self->hosts, i);
    GtkRoot *root = gtk_widget_get_root(widget);
    if (root && GTK_IS_WINDOW(root) && gtk_window_is_active(GTK_WINDOW(root)) &&
        gtk_widget_has_focus(widget) && gtk_widget_get_mapped(widget)) return TRUE;
  }
  return FALSE;
}
GnosisCompositor *gnosis_compositor_new(GError **error) {
  GnosisCompositor *self = g_object_new(GNOSIS_TYPE_COMPOSITOR, NULL);
  self->display = wl_display_create();
  const char *drm = g_getenv("GNOSIS_RENDER_DEVICE");
  self->drm_fd = open(drm ? drm : "/dev/dri/renderD128", O_RDWR | O_CLOEXEC);
  if (self->drm_fd < 0) goto failed;
  self->renderer = wlr_gles2_renderer_create_with_drm_fd(self->drm_fd);
  if (!self->renderer || !wlr_renderer_init_wl_display(self->renderer, self->display)) goto failed;
  struct wlr_compositor *compositor = wlr_compositor_create(self->display, 6, self->renderer);
  listen(&compositor->events.new_surface, &self->new_surface, new_surface);
  wlr_subcompositor_create(self->display); wlr_viewporter_create(self->display);
  wlr_data_device_manager_create(self->display);
  if (!wlr_linux_dmabuf_v1_create_with_renderer(self->display, 4, self->renderer)) goto failed;
  struct wlr_xdg_shell *shell = wlr_xdg_shell_create(self->display, 6);
  listen(&shell->events.new_toplevel, &self->new_toplevel, new_toplevel);
  listen(&shell->events.new_popup, &self->new_popup, new_popup);
  struct wlr_xdg_decoration_manager_v1 *decorations = wlr_xdg_decoration_manager_v1_create(self->display);
  listen(&decorations->events.new_toplevel_decoration, &self->decoration, new_decoration);
  self->backend = wlr_headless_backend_create(wl_display_get_event_loop(self->display));
  if (!self->backend) goto failed;
  if (!wlr_presentation_create(self->display, self->backend, 2)) goto failed;
  self->output = wlr_headless_add_output(self->backend, 1280, 800);
  if (!self->output) goto failed;
  struct wlr_output_state state; wlr_output_state_init(&state);
  GListModel *monitors = gdk_display_get_monitors(gdk_display_get_default());
  GdkMonitor *monitor = g_list_model_get_item(monitors, 0);
  int refresh = monitor ? gdk_monitor_get_refresh_rate(monitor) : 0;
  g_clear_object(&monitor);
  wlr_output_state_set_enabled(&state, TRUE); wlr_output_state_set_custom_mode(&state, 1280, 800, refresh > 0 ? refresh : 60000);
  gboolean committed = wlr_output_commit_state(self->output, &state); wlr_output_state_finish(&state);
  if (!committed || !wlr_backend_start(self->backend)) goto failed;
  struct wlr_output_layout *layout = wlr_output_layout_create(self->display);
  wlr_output_layout_add(layout, self->output, 0, 0); wlr_output_create_global(self->output, self->display);
  wlr_xdg_output_manager_v1_create(self->display, layout);
  self->seat = wlr_seat_create(self->display, "Gnosis GTK seat");
  listen(&self->seat->events.request_set_cursor, &self->request_cursor, request_cursor);
  struct wlr_cursor_shape_manager_v1 *cursor_shapes = wlr_cursor_shape_manager_v1_create(self->display, 1);
  listen(&cursor_shapes->events.request_set_shape, &self->request_cursor_shape, request_cursor_shape);
  listen(&self->seat->events.request_start_drag, &self->request_drag, request_drag);
  self->clipboard = gnosis_clipboard_bridge_new(self->display, self->seat,
    gdk_display_get_clipboard(gdk_display_get_default()), clipboard_allowed, self);
  self->constraints = wlr_pointer_constraints_v1_create(self->display);
  self->relative = wlr_relative_pointer_manager_v1_create(self->display);
  listen(&self->constraints->events.new_constraint, &self->new_constraint, new_constraint);
  static const struct wlr_keyboard_impl keyboard_impl = {.name = "GTK"};
  wlr_keyboard_init(&self->keyboard, &keyboard_impl, "GTK keyboard"); self->keyboard_ready = TRUE;
  struct xkb_context *context = xkb_context_new(XKB_CONTEXT_NO_FLAGS);
  struct xkb_keymap *keymap = xkb_keymap_new_from_names(context, NULL, XKB_KEYMAP_COMPILE_NO_FLAGS);
  if (!keymap || !wlr_keyboard_set_keymap(&self->keyboard, keymap)) { if (keymap) xkb_keymap_unref(keymap); xkb_context_unref(context); goto failed; }
  xkb_keymap_unref(keymap); xkb_context_unref(context);
  wlr_keyboard_set_repeat_info(&self->keyboard, 25, 600);
  wlr_seat_set_keyboard(self->seat, &self->keyboard);
  wlr_seat_set_capabilities(self->seat, WL_SEAT_CAPABILITY_KEYBOARD | WL_SEAT_CAPABILITY_POINTER | WL_SEAT_CAPABILITY_TOUCH);
  self->directory = g_build_filename(g_get_user_runtime_dir(), "gnosis-wayland-XXXXXX", NULL);
  if (!g_mkdtemp(self->directory)) goto failed;
  self->socket = g_build_filename(self->directory, "wayland", NULL);
  if (wl_display_add_socket(self->display, self->socket) < 0) goto failed;
  self->dispatch = gnosis_wayland_source_attach(self->display);
  return self;
failed:
  g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_FAILED, "Could not initialize Gnosis's GPU Wayland compositor");
  g_object_unref(self); return NULL;
}

const char *gnosis_compositor_get_socket(GnosisCompositor *self) { return self->socket; }
GtkWidget *gnosis_compositor_view_new_for_server(GnosisCompositor *server, const char *token) {
  GnosisCompositorView *self = g_object_new(GNOSIS_TYPE_COMPOSITOR_VIEW, NULL);
  self->server = g_object_ref(server); self->token = g_strdup(token);
  g_ptr_array_add(server->hosts, self);
  return GTK_WIDGET(self);
}
GtkWidget *gnosis_compositor_view_new(GError **error) {
  GnosisCompositor *server = gnosis_compositor_new(error); if (!server) return NULL;
  GtkWidget *widget = gnosis_compositor_view_new_for_server(server, NULL);
  g_object_unref(server); return widget;
}
const char *gnosis_compositor_view_get_socket(GnosisCompositorView *self) { return self->server->socket; }
gboolean gnosis_compositor_view_has_surface(GnosisCompositorView *self) {
  return self->active && self->active->toplevel->base->surface->mapped;
}
gboolean gnosis_compositor_view_edit(GnosisCompositorView *self, const char *command) {
  if (!self->active || !gtk_widget_get_mapped(GTK_WIDGET(self))) return FALSE;
  guint code = 0;
  gboolean shift = FALSE;
  if (!g_ascii_strcasecmp(command, "Copy")) code = KEY_C;
  else if (!g_ascii_strcasecmp(command, "Cut")) code = KEY_X;
  else if (!g_ascii_strcasecmp(command, "Paste")) code = KEY_V;
  else if (!g_ascii_strcasecmp(command, "PasteAsPlainText")) { code = KEY_V; shift = TRUE; }
  else if (!g_ascii_strcasecmp(command, "SelectAll")) code = KEY_A;
  else if (!g_ascii_strcasecmp(command, "Undo")) code = KEY_Z;
  else if (!g_ascii_strcasecmp(command, "Redo")) { code = KEY_Z; shift = TRUE; }
  else return FALSE;
  gtk_widget_grab_focus(GTK_WIDGET(self));
  focus_entered(NULL, self);
  struct wlr_keyboard *keyboard = &self->server->keyboard;
  struct wlr_keyboard_modifiers saved = keyboard->modifiers, modifiers = saved;
  xkb_mod_index_t ctrl = xkb_keymap_mod_get_index(keyboard->keymap, XKB_MOD_NAME_CTRL);
  xkb_mod_index_t shift_index = xkb_keymap_mod_get_index(keyboard->keymap, XKB_MOD_NAME_SHIFT);
  if (ctrl == XKB_MOD_INVALID || shift_index == XKB_MOD_INVALID) return FALSE;
  modifiers.depressed = (xkb_mod_mask_t)1 << ctrl;
  if (shift) modifiers.depressed |= (xkb_mod_mask_t)1 << shift_index;
  modifiers.latched = 0;
  wlr_seat_keyboard_notify_modifiers(self->server->seat, &modifiers);
  uint32_t time = (uint32_t)(g_get_monotonic_time() / 1000);
  wlr_seat_keyboard_notify_key(self->server->seat, time, code, WL_KEYBOARD_KEY_STATE_PRESSED);
  wlr_seat_keyboard_notify_key(self->server->seat, time, code, WL_KEYBOARD_KEY_STATE_RELEASED);
  wlr_seat_keyboard_notify_modifiers(self->server->seat, &saved);
  wl_display_flush_clients(self->server->display);
  return TRUE;
}
gboolean gnosis_compositor_view_context_position(GnosisCompositorView *self, double *x, double *y) {
  if (!self->context_time || g_get_monotonic_time() - self->context_time > 2 * G_USEC_PER_SEC) return FALSE;
  *x = self->context_x; *y = self->context_y; return TRUE;
}
