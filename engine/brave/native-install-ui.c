/* SPDX-License-Identifier: GPL-3.0-or-later
 * Exact-binary adapter for the prebuilt browser's extension install dialog.
 * Native Views retains its model, ownership and decision callbacks. Only its
 * final Show operation is replaced with a private GTK-shell IPC request.
 */
#define _GNU_SOURCE
#include <dlfcn.h>
#include <errno.h>
#include <fcntl.h>
#include <glib-unix.h>
#include <gio/gio.h>
#include <json-glib/json-glib.h>
#include <stdint.h>
#include <stdbool.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/socket.h>
#include <unistd.h>
#include "gnosis-native-install-config.h"

typedef void (*NativeVoid)(void *);
typedef struct Request Request;
struct Request {
  Request *next;
  void *widget, *view, *checkbox;
  gint64 id, created;
  gboolean permission, once, chooser, file_access, uninstall, authenticator, external;
  guint64 revision;
};
static uintptr_t image;
static int channel = -1;
static Request *requests;
static gint64 sequence;
static NativeVoid original_destroy, original_permission_destroy, original_permission_show;
static guint input_watch;
static GMutex portal_parent_mutex;
static char *portal_parent;

char *gnosis_portal_parent(void) {
  g_mutex_lock(&portal_parent_mutex);
  char *copy = g_strdup(portal_parent);
  g_mutex_unlock(&portal_parent_mutex);
  return copy;
}

gboolean gnosis_adapter_enabled(const char *id) {
  static gsize initialized;
  static GHashTable *disabled;
  if (g_once_init_enter(&initialized)) {
    disabled = g_hash_table_new_full(g_str_hash,g_str_equal,g_free,NULL);
    g_auto(GStrv) values = g_strsplit(g_getenv("GNOSIS_DISABLED_ADAPTERS") ? g_getenv("GNOSIS_DISABLED_ADAPTERS") : "",",",-1);
    for (guint i=0;values[i];++i) if (*values[i]) g_hash_table_add(disabled,g_strdup(values[i]));
    g_once_init_leave(&initialized,1);
  }
  return !g_hash_table_contains(disabled,id);
}

static void *pointer_at(void *object, size_t offset) {
  void *value;
  memcpy(&value, (char *)object + offset, sizeof value);
  return value;
}

static void native_call(uintptr_t offset, void *object) {
  ((NativeVoid)(image + offset))(object);
}

static gboolean send_object(JsonObject *object) {
  g_autoptr(JsonNode) node = json_node_new(JSON_NODE_OBJECT);
  json_node_set_object(node, object);
  g_autofree char *json = json_to_string(node, FALSE);
  size_t length = strlen(json);
  return length <= 60000 && send(channel, json, length, MSG_NOSIGNAL | MSG_DONTWAIT) == (ssize_t)length;
}

/* Chromium's pinned alternate libc++ u16string layout. Strings returned by
 * native getters are destroyed by the binary's own sized-delete operation.
 * No libstdc++ objects cross the boundary. */
static char *copy_u16(const unsigned char value[24]) {
  size_t length;
  const gunichar2 *data;
  if (value[23] & 0x80) {
    memcpy(&data, value, sizeof data);
    memcpy(&length, value + 8, sizeof length);
  } else {
    data = (const gunichar2 *)value;
    length = value[23];
  }
  if (length > 8192 || (!data && length)) return NULL;
  return g_utf16_to_utf8(data, length, NULL, NULL, NULL);
}

static char *native_label(uintptr_t offset, void *prompt) {
  unsigned char value[24] __attribute__((aligned(8))) = {0};
  ((void (*)(void *, void *))(image + offset))(value, prompt);
  char *result = copy_u16(value);
  if (value[23] & 0x80) {
    size_t capacity;
    memcpy(&capacity, value + 16, sizeof capacity);
    ((void (*)(void *, size_t))(image + UI_SIZED_DELETE_RVA))
      (pointer_at(value, 0), capacity * sizeof(gunichar2));
  }
  return result;
}

static void notify_closed(gint64 id) {
  g_autoptr(JsonObject) message = json_object_new();
  json_object_set_string_member(message, "kind", "closed");
  json_object_set_int_member(message, "id", id);
  send_object(message);
}

static Request *remove_request(gint64 id, void *view) {
  Request **link = &requests;
  while (*link) {
    Request *request = *link;
    if ((id && request->id == id) || (view && request->view == view)) {
      *link = request->next;
      return request;
    }
    link = &request->next;
  }
  return NULL;
}

static void destroy_dialog(void *view) {
  Request *request = remove_request(0, view);
  if (request) { notify_closed(request->id); g_free(request); }
  original_destroy(view);
}

static void destroy_permission(void *view) {
  Request *request = remove_request(0, view);
  if (request) { notify_closed(request->id); g_free(request); }
  original_permission_destroy(view);
}

static void decide_external(Request *, gboolean, const char *);
static void decide_chooser(Request *, gboolean, const char *);
static void decide_file_access(Request *, gboolean);
static void decide_uninstall(Request *, gboolean, const char *);
static void decide_authenticator(Request *, gboolean, const char *);

static void decide(Request *request, gboolean accept, const char *choice) {
  if (request->external) { decide_external(request, accept, choice); return; }
  if (request->authenticator) { decide_authenticator(request, accept, choice); return; }
  if (request->chooser) { decide_chooser(request, accept, choice); return; }
  if (request->file_access) { decide_file_access(request, accept); return; }
  if (request->uninstall) { decide_uninstall(request, accept, choice); return; }
  if (request->permission) {
    /* The native manager retires its prompt after a decision. Do not touch
     * the view/widget after this call; callbacks can destroy them. */
    if (accept && (!choice || strcmp(choice, "once") || request->once)) {
      void *table = pointer_at(request->view, 0);
      ((void (*)(void *, int))pointer_at(table, 0x3f8))
        (request->view, choice && !strcmp(choice, "once") ? 1 : 0);
    } else if (choice && !strcmp(choice, "block")) {
      void *table = pointer_at(request->view, 0);
      ((void (*)(void *, int))pointer_at(table, 0x3f8))(request->view, 2);
    } else native_call(UI_PERMISSION_DISMISS_RVA, request->view);
    g_free(request);
    return;
  }
  /* Close() schedules destruction. Retire the native callback immediately on
   * this UI thread; the destructor then sees an already-completed callback. */
  native_call(UI_WIDGET_CLOSE_RVA, request->widget);
  native_call(accept ? UI_ACCEPT_RVA : UI_CANCEL_RVA, request->view);
  g_free(request);
}


/* Translation keeps the upstream script, service, per-tab LanguageState and
 * revert operation. Only the Views bubble is replaced. All objects below are
 * resolved again from the private CDP target for every request. */
typedef struct {
  void *manager, *client;
  int sequence, step, error;
} Translation;
static GHashTable *translations;

static char *copy_language(const unsigned char *value) {
  size_t length = value[23];
  const char *data = (const char *)value;
  if (length & 0x80) { data = pointer_at((void *)value, 0); memcpy(&length, value + 8, 8); }
  if (length > 64 || (!data && length) || !g_utf8_validate(data, length, NULL)) return g_strdup("");
  return g_strndup(data, length);
}
static gboolean translate_ui(void *client, int step, void *source, void *target, int error, gboolean manual) {
  (void)source; (void)target; (void)manual;
  if (translations) {
    GHashTableIter iter; gpointer value;
    g_hash_table_iter_init(&iter, translations);
    while (g_hash_table_iter_next(&iter, NULL, &value)) {
      Translation *translation = value;
      if (translation->client == client) { translation->step = error ? 4 : step; translation->error = error; }
    }
  }
  return FALSE;
}
static void translation_request(JsonObject *message) {
  const char *target = json_object_get_string_member_with_default(message, "target", "");
  const char *operation = json_object_get_string_member_with_default(message, "operation", "status");
  gint64 id = json_object_get_int_member_with_default(message, "id", 0);
  if (id <= 0 || !*target || strlen(target) > 128) return;
  g_autoptr(JsonObject) result = json_object_new();
  json_object_set_string_member(result, "kind", "translation");
  json_object_set_int_member(result, "id", id);
  g_autofree char *source = NULL;
  g_autofree char *current = NULL;
  const char *failure = "This page is not available for translation";
  void *agent = NULL, *manager = NULL, *client = NULL;
  unsigned char value[24] __attribute__((aligned(8))) = {0};
  size_t length = strlen(target);
  if (length <= 22) { memcpy(value, target, length); value[23] = length; }
  else { memcpy(value, &target, 8); memcpy(value + 8, &length, 8); value[23] = 0x80; }
  agent = ((void *(*)(const void *))(image + UI_AGENT_FOR_ID_RVA))(value);
  if (!agent) goto done;
  void *contents = ((void *(*)(void *))pointer_at(pointer_at(agent, 0), 0x60))(agent);
  if (!contents) goto done;
  void *data = ((void *(*)(void *, void *))(image + UI_GET_USER_DATA_RVA))
    ((char *)contents + 8, (void *)(image + UI_TRANSLATE_CLIENT_KEY_RVA));
  if (!data || pointer_at(data, 0) != (void *)(image + UI_TRANSLATE_DATA_VTABLE_RVA)) goto done;
  manager = pointer_at(data, 0x20);
  void *driver = pointer_at(data, 0x10);
  if (!manager || !driver || pointer_at(manager, 0x18) != driver || pointer_at(driver, 0x60) != contents) goto done;
  client = pointer_at(manager, 0x10);
  if (!client || pointer_at(client, 0) != (void *)(image + UI_TRANSLATE_CLIENT_VTABLE_RVA)) goto done;
  void *url = ((void *(*)(void *))(image + UI_TRANSLATE_VISIBLE_URL_RVA))(driver);
  if (!((bool (*)(void *, void *))(image + UI_TRANSLATE_URL_ALLOWED_RVA))(client, url)) goto done;
  int page_sequence; memcpy(&page_sequence, (char *)manager + 8, sizeof page_sequence);
  if (!translations) translations = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, g_free);
  Translation *state = g_hash_table_lookup(translations, target);
  if (!state || state->manager != manager || state->sequence != page_sequence) {
    if (g_hash_table_size(translations) > 512) g_hash_table_remove_all(translations);
    state = g_new0(Translation, 1); state->manager = manager; state->client = client;
    state->sequence = page_sequence; state->step = 0;
    g_hash_table_replace(translations, g_strdup(target), state);
  }
  if (strcmp(operation, "status")) {
    if (json_object_get_int_member_with_default(message, "sequence", -1) != page_sequence) {
      failure = "The page changed. Open translation again."; goto done;
    }
    if (!strcmp(operation, "start")) {
      const char *language = json_object_get_string_member_with_default(message, "language", "");
      size_t size = strlen(language);
      if (!size || size > 32 || !((bool (*)(const char *, size_t))(image + UI_TRANSLATE_SUPPORTED_RVA))(language, size)) {
        failure = "This translation language is not supported"; goto done;
      }
      if (state->step == 1) { failure = "A translation is already in progress"; goto done; }
      g_autofree char *source = copy_language((unsigned char *)manager + 0x50);
      gboolean translated = ((bool (*)(void *))(image + UI_TRANSLATE_IS_TRANSLATED_RVA))((char *)manager + 0x48);
      state->step = 1; state->error = 0;
      ((void (*)(void *, const char *, size_t, const char *, size_t, gboolean, int))(image + UI_TRANSLATE_PAGE_RVA))
        (manager, *source ? source : "und", *source ? strlen(source) : 3, language, size, TRUE, translated ? 6 : 5);
    } else if (!strcmp(operation, "revert")) {
      if (state->step == 1) { failure = "Wait for translation to finish"; goto done; }
      native_call(UI_TRANSLATE_REVERT_RVA, manager); state->step = 3; state->error = 0;
    } else { failure = "Unknown translation operation"; goto done; }
  }
  json_object_set_int_member(result, "sequence", page_sequence);
  json_object_set_int_member(result, "step", state->step);
  json_object_set_int_member(result, "errorCode", state->error);
  source = copy_language((unsigned char *)manager + 0x50);
  current = copy_language((unsigned char *)manager + 0x68);
  json_object_set_string_member(result, "source", source);
  json_object_set_string_member(result, "current", current);
  json_object_set_boolean_member(result, "translated", ((bool (*)(void *))(image + UI_TRANSLATE_IS_TRANSLATED_RVA))((char *)manager + 0x48));
  failure = NULL;
done:
  if (failure) json_object_set_string_member(result, "error", failure);
  send_object(result);
  if (agent) {
    unsigned *references = (unsigned *)((char *)agent + 8);
    if (!--*references) ((NativeVoid)pointer_at(pointer_at(agent, 0), 0xe8))(agent);
  }
}

static gboolean native_close_request(JsonObject *message);
static bool native_find_dispatch(JsonObject *message);
static void native_find_shutdown(void);
static void authenticator_owner(JsonObject *message);
static gboolean receive_response(gint fd, GIOCondition condition, gpointer unused) {
  (void)unused;
  if (condition & G_IO_IN) {
    char data[65536];
    ssize_t length = recv(fd, data, sizeof data, MSG_DONTWAIT | MSG_TRUNC);
    if (length > 0 && length <= (ssize_t)sizeof data) {
      g_autoptr(JsonParser) parser = json_parser_new();
      if (json_parser_load_from_data(parser, data, length, NULL) &&
          JSON_NODE_HOLDS_OBJECT(json_parser_get_root(parser))) {
        JsonObject *message = json_node_get_object(json_parser_get_root(parser));
        if (!strcmp(json_object_get_string_member_with_default(message, "kind", ""), "parent-window")) {
          const char *handle = json_object_get_string_member_with_default(message, "handle", "");
          if (strlen(handle) <= 4096 && (!*handle || g_str_has_prefix(handle, "wayland:") || g_str_has_prefix(handle, "x11:"))) {
            g_mutex_lock(&portal_parent_mutex);
            g_free(portal_parent); portal_parent = g_strdup(handle);
            g_mutex_unlock(&portal_parent_mutex);
          }
          return G_SOURCE_CONTINUE;
        }
        if (native_close_request(message) || native_find_dispatch(message)) return G_SOURCE_CONTINUE;
        if (!strcmp(json_object_get_string_member_with_default(message, "kind", ""), "translation")) {
          translation_request(message); return G_SOURCE_CONTINUE;
        }
        if (!strcmp(json_object_get_string_member_with_default(message, "kind", ""), "authenticator-owner")) {
          authenticator_owner(message); return G_SOURCE_CONTINUE;
        }
        JsonNode *id = json_object_get_member(message, "id");
        JsonNode *accept = json_object_get_member(message, "accept");
        if (id && accept && JSON_NODE_HOLDS_VALUE(id) && JSON_NODE_HOLDS_VALUE(accept) &&
            json_node_get_value_type(id) == G_TYPE_INT64 && json_node_get_value_type(accept) == G_TYPE_BOOLEAN) {
          Request *request = remove_request(json_node_get_int(id), NULL);
          if (request && request->authenticator && json_node_get_boolean(accept) &&
              g_get_monotonic_time() - request->created < 500000) {
            request->next = requests; requests = request;
            return G_SOURCE_CONTINUE;
          }
          if (request) decide(request, json_node_get_boolean(accept) &&
            g_get_monotonic_time() - request->created >= 500000,
            json_object_get_string_member_with_default(message, "choice", ""));
        }
      }
      return G_SOURCE_CONTINUE;
    }
    if (length < 0 && (errno == EAGAIN || errno == EINTR)) return G_SOURCE_CONTINUE;
    condition |= G_IO_HUP;
  }
  if (condition & (G_IO_HUP | G_IO_ERR)) {
    input_watch = 0;
    native_find_shutdown();
    while (requests) decide(remove_request(requests->id, NULL), FALSE, "");
    close(channel); channel = -1;
    return G_SOURCE_REMOVE;
  }
  return G_SOURCE_CONTINUE;
}

static void show_install(void *widget, void *view) {
  void *prompt = pointer_at(view, 0x780);
  int kind;
  memcpy(&kind, prompt, sizeof kind);
  /* Enterprise justification and host-withholding variants need additional
   * controls. Keep native handling until those controls have a GTK bridge. */
  if (!gnosis_adapter_enabled("extension-dialogs") || channel < 0 || (kind != 0 && kind != 3 && kind != 4) ||
      pointer_at(view, 0x818) || pointer_at(view, 0x820)) {
    native_call(UI_WIDGET_SHOW_RVA, widget); return;
  }
  g_autofree char *title = native_label(UI_TITLE_RVA, prompt);
  g_autofree char *accept = native_label(UI_ACCEPT_LABEL_RVA, prompt);
  g_autofree char *cancel = native_label(UI_CANCEL_LABEL_RVA, prompt);
  g_autofree char *heading = native_label(UI_HEADING_RVA, prompt);
  uintptr_t begin = (uintptr_t)pointer_at(prompt, 0x20);
  /* This pinned Chromium libc++ vector stores data, size, capacity rather
   * than begin/end pointers (verified in native GetPermission). */
  size_t count;
  memcpy(&count, (char *)prompt + 0x28, sizeof count);
  if (!title || !accept || !*accept || !cancel || !heading ||
      count > 256 || (count && !begin)) {
    native_call(UI_WIDGET_SHOW_RVA, widget); return;
  }
  g_autoptr(JsonObject) message = json_object_new();
  JsonArray *warnings = json_array_new();
  json_object_set_array_member(message, "permissions", warnings);
  for (uintptr_t item = begin; item < begin + count * 24; item += 24) {
    g_autofree char *warning = copy_u16((void *)item);
    if (!warning) { native_call(UI_WIDGET_SHOW_RVA, widget); return; }
    json_array_add_string_element(warnings, warning);
  }
  Request *request = g_new0(Request, 1);
  request->id = ++sequence; request->created = g_get_monotonic_time();
  request->widget = widget; request->view = view;
  json_object_set_string_member(message, "kind", "install");
  json_object_set_int_member(message, "id", request->id);
  json_object_set_string_member(message, "title", title);
  json_object_set_string_member(message, "accept", accept);
  json_object_set_string_member(message, "cancel", cancel);
  json_object_set_string_member(message, "heading", heading);
  if (!send_object(message)) { g_free(request); native_call(UI_WIDGET_SHOW_RVA, widget); return; }
  request->next = requests; requests = request;
  if (!input_watch) input_watch = g_unix_fd_add(channel, G_IO_IN | G_IO_HUP | G_IO_ERR, receive_response, NULL);
}

static void show_permission(void *view) {
  if (!gnosis_adapter_enabled("permission-dialogs") || channel < 0) { original_permission_show(view); return; }
  void *widget = ((void *(*)(void *))(image + UI_PERMISSION_GET_WIDGET_RVA))(view);
  void *delegate = (char *)view + 0x310;
  void *table = pointer_at(delegate, 0);
  /* WidgetDelegate::GetAccessibleWindowTitle returns the browser's own
   * localized origin + permission description, including combined requests. */
  uintptr_t title_getter = (uintptr_t)pointer_at(table, 0x88) - image;
  g_autofree char *title = native_label(title_getter, delegate);
  if (!widget || !title || !*title) { original_permission_show(view); return; }
  Request *request = g_new0(Request, 1);
  request->permission = TRUE;
  request->once = *((unsigned char *)view + 0x82c) != 0;
  request->id = ++sequence; request->created = g_get_monotonic_time();
  request->widget = widget; request->view = view;
  g_autoptr(JsonObject) message = json_object_new();
  json_object_set_string_member(message, "kind", "permission");
  json_object_set_int_member(message, "id", request->id);
  json_object_set_string_member(message, "title", title);
  json_object_set_boolean_member(message, "once", request->once);
  if (!send_object(message)) { g_free(request); original_permission_show(view); return; }
  request->next = requests; requests = request;
  if (!input_watch) input_watch = g_unix_fd_add(channel, G_IO_IN | G_IO_HUP | G_IO_ERR, receive_response, NULL);
}

static void request_relaunch(void) {
  g_autoptr(JsonObject) message = json_object_new();
  json_object_set_string_member(message, "kind", "restart");
  /* The shell must recreate its compositor and inherited protocol channels.
   * A standalone native relaunch loses those file descriptors. */
  if (!send_object(message)) native_call(UI_RELAUNCH_ORIGINAL_RVA, NULL);
}

static void absolute_jump(unsigned char *code, void *target) {
  memcpy(code, "\xff\x25\0\0\0\0", 6);
  memcpy(code + 6, &target, sizeof target);
}

static gboolean matches(uintptr_t offset, const char *hex) {
  for (size_t i = 0; hex[i]; i += 2)
    if (*(unsigned char *)(image + offset + i / 2) !=
        (g_ascii_xdigit_value(hex[i]) * 16 + g_ascii_xdigit_value(hex[i + 1]))) return FALSE;
  return TRUE;
}

static gboolean replace_code(uintptr_t offset, const void *code, size_t length) {
  size_t page_size = sysconf(_SC_PAGESIZE);
  uintptr_t page = (image + offset) & ~(page_size - 1);
  size_t span = ((image + offset + length - page + page_size - 1) / page_size) * page_size;
  if (mprotect((void *)page, span, PROT_READ | PROT_WRITE)) return FALSE;
  memcpy((void *)(image + offset), code, length);
  return mprotect((void *)page, span, PROT_READ | PROT_EXEC) == 0;
}


/* Shared native chooser for USB, HID, Serial and Bluetooth. The browser keeps
 * its controller, document checks and device enumeration. Native view callbacks
 * invalidate GTK selections on every mutation, including remove/reinsert with
 * an identical display name. Never grant by a stale UI index. */
typedef struct Chooser Chooser;
struct Chooser { Chooser *next; void *view; guint64 revision; };
static Chooser *choosers;
static void (*original_chooser_construct)(void *, void *, void *);
static NativeVoid original_chooser_destroy, original_widget_show;
static void (*chooser_callbacks[7])(void *, size_t);
static gboolean replace_code(uintptr_t, const void *, size_t);

static Chooser *find_chooser(void *view) {
  for (Chooser *c = choosers; c; c = c->next) if (c->view == view) return c;
  return NULL;
}
static void chooser_construct(void *view, void *observer, void *controller) {
  original_chooser_construct(view, observer, controller);
  Chooser *c = g_new0(Chooser, 1);
  c->view = view; c->revision = 1; c->next = choosers; choosers = c;
}
static void chooser_destroy(void *view) {
  Chooser **link = &choosers;
  while (*link && (*link)->view != view) link = &(*link)->next;
  if (*link) { Chooser *c = *link; *link = c->next; g_free(c); }
  Request *r = remove_request(0, view);
  if (r) { notify_closed(r->id); g_free(r); }
  original_chooser_destroy(view);
}
static char *chooser_option(void *controller, size_t index) {
  unsigned char value[24] __attribute__((aligned(8))) = {0};
  ((void (*)(void *, void *, size_t))pointer_at(pointer_at(controller, 0), 0x88))(value, controller, index);
  char *label = copy_u16(value);
  if (value[23] & 0x80) {
    size_t capacity; memcpy(&capacity, value + 16, 8);
    ((void (*)(void *, size_t))(image + UI_SIZED_DELETE_RVA))(pointer_at(value, 0), capacity * 2);
  }
  return label;
}
static gboolean publish_chooser(Request *r, Chooser *c) {
  void *controller = pointer_at(c->view, 0x320), *table = pointer_at(controller, 0);
  size_t count = ((size_t (*)(void *))pointer_at(table, 0x78))(controller);
  if (count > 256) return FALSE;
  g_autofree char *title = native_label(UI_CHOOSER_TITLE_RVA, c->view);
  g_autofree char *accept = native_label((uintptr_t)pointer_at(table, 0x40) - image, controller);
  g_autofree char *cancel = native_label((uintptr_t)pointer_at(table, 0x48) - image, controller);
  if (!title || !accept || !cancel) return FALSE;
  g_autoptr(JsonObject) message = json_object_new();
  json_object_set_string_member(message, "kind", "chooser");
  json_object_set_int_member(message, "id", r->id);
  json_object_set_int_member(message, "revision", c->revision);
  json_object_set_string_member(message, "title", title);
  json_object_set_string_member(message, "accept", accept);
  json_object_set_string_member(message, "cancel", cancel);
  json_object_set_string_member(message, "heading", "");
  json_object_set_array_member(message, "permissions", json_array_new());
  json_object_set_boolean_member(message, "multiple", ((gboolean (*)(void *))pointer_at(table, 0x28))(controller));
  json_object_set_boolean_member(message, "enabled", *((unsigned char *)c->view + 0x328) && *((unsigned char *)c->view + 0x329));
  JsonArray *options = json_array_new(); json_object_set_array_member(message, "options", options);
  for (size_t i = 0; i < count; ++i) {
    g_autofree char *label = chooser_option(controller, i);
    if (!label) return FALSE;
    json_array_add_string_element(options, label);
  }
  r->revision = c->revision;
  return send_object(message);
}
static void chooser_changed(unsigned slot, void *subview, size_t argument) {
  void *view = (char *)subview - 0x318;
  Chooser *c = find_chooser(view);
  if (c) ++c->revision;
  chooser_callbacks[slot](subview, argument);
  /* Native callbacks may retire the view. Re-find without dereferencing it. */
  c = find_chooser(view);
  if (c) for (Request *r = requests; r; r = r->next)
    if (r->chooser && r->view == view) { publish_chooser(r, c); break; }
}
#define CHOOSER_CALLBACK(n) static void chooser_changed_##n(void *v, size_t a) { chooser_changed(n, v, a); }
CHOOSER_CALLBACK(0) CHOOSER_CALLBACK(1) CHOOSER_CALLBACK(2) CHOOSER_CALLBACK(3)
CHOOSER_CALLBACK(4) CHOOSER_CALLBACK(5) CHOOSER_CALLBACK(6)
static gboolean show_file_access(void *widget);
static gboolean show_portal_capture(void *widget);
static gboolean show_uninstall(void *widget);
static gboolean show_authenticator_widget(void *widget);

static void show_widget(void *widget) {
  if (gnosis_adapter_enabled("passkeys") && show_authenticator_widget(widget)) return;
  if ((gnosis_adapter_enabled("file-access") && show_file_access(widget)) ||
      (gnosis_adapter_enabled("screen-capture") && show_portal_capture(widget)) ||
      (gnosis_adapter_enabled("extension-dialogs") && show_uninstall(widget))) return;
  if (gnosis_adapter_enabled("device-choosers") && channel >= 0) for (Chooser *c = choosers; c; c = c->next) {
    void *owner = ((void *(*)(void *))(image + UI_PERMISSION_GET_WIDGET_RVA))(c->view);
    if (owner != widget) continue;
    for (Request *r = requests; r; r = r->next) if (r->view == c->view) return;
    Request *r = g_new0(Request, 1);
    r->id = ++sequence; r->created = g_get_monotonic_time();
    r->chooser = TRUE; r->view = c->view; r->widget = widget;
    if (!publish_chooser(r, c)) { g_free(r); break; }
    r->next = requests; requests = r;
    if (!input_watch) input_watch = g_unix_fd_add(channel, G_IO_IN | G_IO_HUP | G_IO_ERR, receive_response, NULL);
    return;
  }
  original_widget_show(widget);
}
static void decide_chooser(Request *r, gboolean accept, const char *choice) {
  Chooser *c = find_chooser(r->view);
  size_t indices[256], count = 0;
  if (c && accept && choice && g_str_has_prefix(choice, "devices:")) {
    char *end; guint64 revision = g_ascii_strtoull(choice + 8, &end, 10);
    if (revision != c->revision || revision != r->revision || *end != ':') accept = FALSE;
    else {
      const char *at = end + 1;
      void *controller = pointer_at(c->view, 0x320), *table = pointer_at(controller, 0);
      size_t options = ((size_t (*)(void *))pointer_at(table, 0x78))(controller);
      while (*at && count < G_N_ELEMENTS(indices)) {
        guint64 index = g_ascii_strtoull(at, &end, 10);
        if (end == at || index >= options || (count && index <= indices[count - 1]) || (*end && *end != ',')) { accept = FALSE; break; }
        indices[count++] = index; at = *end ? end + 1 : end;
      }
      if (*at || !count || (count > 1 && !((gboolean (*)(void *))pointer_at(table, 0x28))(controller)) ||
          !*((unsigned char *)c->view + 0x328) || !*((unsigned char *)c->view + 0x329)) accept = FALSE;
    }
  } else accept = FALSE;
  if (c) {
    void *controller = pointer_at(c->view, 0x320), *table = pointer_at(controller, 0);
    /* Same alternate libc++ data/size/capacity vector used by native Accept. */
    struct { size_t *data, size, capacity; } vector = {indices, count, count};
    void *widget = r->widget;
    if (accept) ((void (*)(void *, void *))pointer_at(table, 0xa8))(controller, &vector);
    else native_call(UI_CHOOSER_CANCEL_RVA, r->view);
    native_call(UI_WIDGET_CLOSE_RVA, widget);
  }
  g_free(r);
}


/* File-system access has a separate DialogModel after the portal chooses a
 * path. Keep its real native host alive; GTK only replaces Widget::Show. */
static void *pending_file_model, *pending_file_host;
static void *(*original_create_modal)(void *, int, gboolean, gboolean);
static NativeVoid original_model_host_destroy;
static void *create_modal(void *model, int modal, gboolean autosize, gboolean owned) {
  void *host = original_create_modal(model, modal, autosize, owned);
  if (model == pending_file_model) pending_file_host = host;
  return host;
}
/* Restrict the generic extension-model host to the pinned uninstall call site.
 * Added-extension notifications and unrelated browser dialogs retain their path. */
static void *pending_uninstall_model;
typedef struct UninstallModel UninstallModel;
struct UninstallModel { UninstallModel *next; void *host, *model; };
static UninstallModel *uninstall_models;
static void (*original_extension_model_show)(void *, void *, void *);
static void (*original_model_host_construct)(void *, void *, void *, int, int, int, int);
static void show_extension_model(void *parent, void *extension, void *model) {
  gboolean uninstall = (uintptr_t)__builtin_return_address(0) == image + UI_UNINSTALL_MODEL_CALLER_RVA;
  void *previous_model = pending_uninstall_model;
  if (uninstall && gnosis_adapter_enabled("extension-dialogs")) pending_uninstall_model = model;
  original_extension_model_show(parent, extension, model);
  pending_uninstall_model = previous_model;
}
static void model_host_construct(void *host, void *model, void *anchor, int arrow, int autosize, int modal, int owned) {
  original_model_host_construct(host, model, anchor, arrow, autosize, modal, owned);
  if (model == pending_uninstall_model) {
    UninstallModel *entry = g_new0(UninstallModel, 1);
    entry->host = host; entry->model = model;
    entry->next = uninstall_models; uninstall_models = entry;
  }
}
static char *model_label(void *label) {
  int message; memcpy(&message, label, 4);
  if (message == -1) return copy_u16((unsigned char *)label + 8);
  unsigned char value[24] __attribute__((aligned(8))) = {0};
  ((void (*)(void *, int))(image + UI_RESOURCE_STRING_RVA))(value, message);
  g_autofree char *format = copy_u16(value);
  if (value[23] & 0x80) {
    size_t capacity; memcpy(&capacity, value + 16, 8);
    ((void (*)(void *, size_t))(image + UI_SIZED_DELETE_RVA))(pointer_at(value, 0), capacity * 2);
  }
  size_t count = (size_t)pointer_at(label, 0x28);
  if (!format || count > 16) return NULL;
  void *replacements = pointer_at(label, 0x20);
  if (count && !replacements) return NULL;
  g_autoptr(GPtrArray) texts = g_ptr_array_new_with_free_func(g_free);
  for (size_t i = 0; i < count; ++i) {
    char *text = copy_u16((unsigned char *)replacements + i * 0x50);
    if (!text) return NULL;
    g_ptr_array_add(texts, text);
  }
  GString *body = g_string_new(NULL);
  for (const char *at = format; *at; ++at) {
    if (*at == '$' && at[1] >= '1' && at[1] <= '9' && (size_t)(at[1] - '1') < count) {
      g_string_append(body, g_ptr_array_index(texts, at[1] - '1')); ++at;
    } else if (*at == '$' && at[1] == '$') { g_string_append_c(body, '$'); ++at; }
    else g_string_append_c(body, *at);
  }
  return g_string_free(body, FALSE);
}
static gboolean show_uninstall(void *widget) {
  if (channel < 0) return FALSE;
  UninstallModel *entry = uninstall_models;
  while (entry && pointer_at(entry->host, 8) != widget) entry = entry->next;
  if (!entry) return FALSE;
  for (Request *r = requests; r; r = r->next) if (r->view == entry->host) return TRUE;
  void *model = entry->model;
  g_autofree char *title = copy_u16((unsigned char *)model + 0x30);
  g_autofree char *accept = copy_u16((unsigned char *)model + 0x2e0);
  g_autofree char *cancel = copy_u16((unsigned char *)model + 0x388);
  if (!title || !accept || !*accept || !cancel) return FALSE;
  size_t count = (size_t)pointer_at(model, 0x230);
  void **fields = pointer_at(model, 0x228), *checkbox = NULL;
  if (count > 8 || (count && !fields)) return FALSE;
  g_autoptr(JsonObject) message = json_object_new();
  JsonArray *paragraphs = json_array_new();
  json_object_set_array_member(message, "permissions", paragraphs);
  for (size_t i = 0; i < count; ++i) {
    int type; memcpy(&type, (unsigned char *)fields[i] + 8, 4);
    if (type != 0 && type != 1) return FALSE;
    g_autofree char *label = model_label((unsigned char *)fields[i] + 0x70);
    if (!label) return FALSE;
    if (type == 0) json_array_add_string_element(paragraphs, label);
    else {
      if (checkbox) return FALSE;
      checkbox = fields[i];
      json_object_set_string_member(message, "checkbox", label);
      json_object_set_boolean_member(message, "checked", *((unsigned char *)checkbox + 0xb0) != 0);
    }
  }
  Request *r = g_new0(Request, 1);
  r->id = ++sequence; r->created = g_get_monotonic_time();
  r->uninstall = TRUE; r->view = entry->host; r->widget = widget; r->checkbox = checkbox;
  json_object_set_string_member(message, "kind", "uninstall");
  json_object_set_int_member(message, "id", r->id);
  json_object_set_string_member(message, "title", title);
  json_object_set_string_member(message, "accept", accept);
  json_object_set_string_member(message, "cancel", cancel);
  json_object_set_string_member(message, "heading", "");
  if (!send_object(message)) { g_free(r); return FALSE; }
  r->next = requests; requests = r;
  if (!input_watch) input_watch = g_unix_fd_add(channel, G_IO_IN | G_IO_HUP | G_IO_ERR, receive_response, NULL);
  return TRUE;
}
static void decide_uninstall(Request *r, gboolean accept, const char *choice) {
  if (r->checkbox) *((unsigned char *)r->checkbox + 0xb0) = accept && choice && !strcmp(choice, "report-abuse");
  native_call(accept ? UI_DIALOG_ACCEPT_RVA : UI_DIALOG_CANCEL_RVA, r->view);
  g_free(r);
}
static void destroy_model_host(void *host) {
  UninstallModel **entry = &uninstall_models;
  while (*entry) {
    if ((*entry)->host == host) { UninstallModel *old = *entry; *entry = old->next; g_free(old); break; }
    entry = &(*entry)->next;
  }
  Request *r = remove_request(0, host);
  if (r) { notify_closed(r->id); g_free(r); }
  original_model_host_destroy(host);
}
static void file_access_dialog(void *request, void *callback, void *contents) {
  void *model = ((void *(*)(void *, void *, void *))(image + UI_FILE_MODEL_CREATE_RVA))(contents, request, callback);
  void *previous_model = pending_file_model, *previous_host = pending_file_host;
  pending_file_model = gnosis_adapter_enabled("file-access") ? model : NULL; pending_file_host = NULL;
  ((void (*)(void *, void *))(image + UI_SHOW_TAB_MODAL_RVA))(model, contents);
  pending_file_model = previous_model; pending_file_host = previous_host;
}
static char *file_access_body(void *model) {
  size_t fields; memcpy(&fields, (char *)model + 0x230, 8);
  void *items = pointer_at(model, 0x228);
  if (fields != 1 || !items) return NULL;
  void *paragraph = pointer_at(items, 0);
  int type; memcpy(&type, (char *)paragraph + 8, 4);
  if (type != 0) return NULL;
  void *label = (char *)paragraph + 0x70;
  size_t count; memcpy(&count, (char *)label + 0x28, 8);
  if (count != 2) return NULL;
  void *replacements = pointer_at(label, 0x20);
  if (!replacements) return NULL;
  g_autofree char *first = copy_u16(replacements);
  g_autofree char *second = copy_u16((unsigned char *)replacements + 0x50);
  if (!first || !second) return NULL;
  unsigned char value[24] __attribute__((aligned(8))) = {0};
  int message; memcpy(&message, label, 4);
  ((void (*)(void *, int))(image + UI_RESOURCE_STRING_RVA))(value, message);
  g_autofree char *format = copy_u16(value);
  if (value[23] & 0x80) {
    size_t capacity; memcpy(&capacity, value + 16, 8);
    ((void (*)(void *, size_t))(image + UI_SIZED_DELETE_RVA))(pointer_at(value, 0), capacity * 2);
  }
  if (!format) return NULL;
  GString *body = g_string_new(NULL);
  /* Native localized resource substitutions. Scan only the template, never
   * replacement text (a directory name may itself contain '$1'). */
  for (const char *at = format; *at; ++at) {
    if (*at == '$' && at[1] == '1') { g_string_append(body, first); ++at; }
    else if (*at == '$' && at[1] == '2') { g_string_append(body, second); ++at; }
    else if (*at == '$' && at[1] == '$') { g_string_append_c(body, '$'); ++at; }
    else g_string_append_c(body, *at);
  }
  return g_string_free(body, FALSE);
}
static gboolean show_file_access(void *widget) {
  if (channel < 0 || !pending_file_host || pointer_at(pending_file_host, 8) != widget) return FALSE;
  void *model = pending_file_model;
  g_autofree char *title = copy_u16((unsigned char *)model + 0x30);
  g_autofree char *accept = copy_u16((unsigned char *)model + 0x2e0);
  g_autofree char *cancel = copy_u16((unsigned char *)model + 0x388);
  g_autofree char *body = file_access_body(model);
  if (!title || !accept || !*accept || !cancel || !*cancel || !body) return FALSE;
  Request *r = g_new0(Request, 1);
  r->id = ++sequence; r->created = g_get_monotonic_time();
  r->file_access = TRUE; r->view = pending_file_host; r->widget = widget;
  g_autoptr(JsonObject) message = json_object_new();
  json_object_set_string_member(message, "kind", "file-access");
  json_object_set_int_member(message, "id", r->id);
  json_object_set_string_member(message, "title", title);
  json_object_set_string_member(message, "accept", accept);
  json_object_set_string_member(message, "cancel", cancel);
  json_object_set_string_member(message, "heading", "");
  JsonArray *warnings = json_array_new(); json_array_add_string_element(warnings, body);
  json_object_set_array_member(message, "permissions", warnings);
  if (!send_object(message)) { g_free(r); return FALSE; }
  r->next = requests; requests = r;
  if (!input_watch) input_watch = g_unix_fd_add(channel, G_IO_IN | G_IO_HUP | G_IO_ERR, receive_response, NULL);
  return TRUE;
}
static void decide_file_access(Request *r, gboolean accept) {
  /* DialogDelegate guards synchronous destruction and suppresses the close
   * callback after a button decision. Calling the model then Widget::Close
   * instead consumes FileSystemAccess's split-once callback twice. */
  native_call(accept ? UI_DIALOG_ACCEPT_RVA : UI_DIALOG_CANCEL_RVA, r->view);
  g_free(r);
}


/* Wayland's delegated desktop list is the real XDG ScreenCast picker. Its
 * explicit selection is already consent; do not put a second Views picker
 * around it. Retain the native picker/model and all capture policy callbacks. */
typedef struct Capture Capture;
struct Capture { Capture *next; void *view; int type; gboolean hidden, combined, selected, completed; };
static Capture *captures;
static gint capture_total, capture_combined;
static void (*original_capture_added)(void *, int);
static void (*original_capture_construct)(void *, void *, void *, void *);
static NativeVoid original_capture_destroy, original_capture_selected, original_capture_dismissed;
static void *(*original_capture_widget)(void *, void *, void *, void *, void *);
static Capture *find_capture(void *view) {
  for (Capture *c = captures; c; c = c->next) if (c->view == view) return c;
  return NULL;
}
static gboolean delegated_list(void *list) {
  return ((gboolean (*)(void *))pointer_at(pointer_at(list, 0), 0x58))(list);
}
static int list_type(void *list) {
  return ((int (*)(void *))pointer_at(pointer_at(list, 0), 0x48))(list);
}
static void capture_construct(void *view, void *params, void *parent, void *lists) {
  int preference; memcpy(&preference, (char *)params + 0x60, 4);
  size_t count; memcpy(&count, (char *)lists + 8, 8);
  void *items = pointer_at(lists, 0);
  int selected = 0;
  unsigned allowed = 0;
  /* Browser-tab-only requests keep their existing path. Never synthesize a
   * screen list when the caller/policy omitted it. Prefer the requested window
   * list; otherwise use the portal's monitor list for no-preference requests. */
  if (gnosis_adapter_enabled("screen-capture") && channel >= 0 && items && count <= 8 && preference != 3) {
    for (size_t i = 0; i < count; ++i) {
      void *list = pointer_at(items, i * 8);
      int type = list_type(list);
      if (type == 1 || type == 2) allowed |= 1u << (type - 1);
      if (delegated_list(list) && (type == 1 || type == 2) &&
          (type == preference || (!preference && (!selected || type == 1)))) selected = type;
    }
  }
  Capture *c = gnosis_adapter_enabled("screen-capture") && channel >= 0 ? g_new0(Capture, 1) : NULL;
  if (c) {
    c->view = view; c->type = selected; c->combined = selected && allowed == 3;
    c->next = captures; captures = c;
    g_atomic_int_inc(&capture_total);
    if (c->combined) g_atomic_int_inc(&capture_combined);
  }
  unsigned char copy[0x80] __attribute__((aligned(8)));
  if (selected) {
    /* A borrowed const Params copy: native construction makes its own owning
     * copies. We change only the presentation preference, not source filters. */
    memcpy(copy, params, sizeof copy); memcpy(copy + 0x60, &selected, 4);
    params = copy;

  }
  original_capture_construct(view, params, parent, lists);
}
static void capture_destroy(void *view) {
  Capture **link = &captures;
  while (*link && (*link)->view != view) link = &(*link)->next;
  if (*link) {
    Capture *c = *link; *link = c->next;
    if (c->combined) g_atomic_int_add(&capture_combined, -1);
    g_atomic_int_add(&capture_total, -1); g_free(c);
  }
  original_capture_destroy(view);
}
static void *capture_controller(Capture *c) {
  size_t count; memcpy(&count, (char *)c->view + 0x648, 8);
  if (!count || count > 8) return NULL;
  void *tabs = pointer_at(c->view, 0x638);
  int index = tabs ? ((int (*)(void *))(image + UI_CAPTURE_TAB_INDEX_RVA))(tabs) : 0;
  if (index < 0 || (size_t)index >= count) return NULL;
  void *category = (char *)pointer_at(c->view, 0x640) + index * 32;
  int type; memcpy(&type, category, 4);
  void *controller = pointer_at(category, 8);
  if (type != c->type || !controller || !delegated_list(pointer_at(controller, 0x18))) return NULL;
  return controller;
}
static void *capture_widget(void *browser, void *contents, void *view, void *context, void *parent) {
  Capture *c = find_capture(view);
  if (!c || !c->type || !capture_controller(c))
    return original_capture_widget(browser, contents, view, context, parent);
  /* Web-modal pickers are shown by WebContentsModalDialogManager, bypassing
   * Widget::Show. Create the same owned widget without registering/showing a
   * second modal: the system portal owns the actual source-selection UI. */
  ((void (*)(void *, int))(image + UI_CAPTURE_SET_MODAL_RVA))(view, 0);
  void *widget = ((void *(*)(void *, void *, void *))(image + UI_CAPTURE_CREATE_WIDGET_RVA))(view, context, parent);
  c->hidden = TRUE;
  g_printerr("Gnosis capture: portal-only widget created without Chromium modal\n");
  return widget;
}
static gboolean show_portal_capture(void *widget) {
  for (Capture *c = captures; c; c = c->next) {
    void *owner = ((void *(*)(void *))(image + UI_PERMISSION_GET_WIDGET_RVA))((char *)c->view + 0x2c8);
    if (c->type && owner == widget && capture_controller(c)) {
      c->hidden = TRUE;
      g_printerr("Gnosis capture: XDG portal owns source selection\n");
      return TRUE;
    }
  }
  return FALSE;
}
static gboolean finish_portal_capture(gpointer view) {
  Capture *c = find_capture(view);
  if (!c || !c->selected || c->completed || !c->hidden) return G_SOURCE_REMOVE;
  void *controller = capture_controller(c);
  if (!controller) return G_SOURCE_REMOVE;
  size_t count = ((size_t (*)(void *))(image + UI_CAPTURE_SOURCE_COUNT_RVA))(controller);
  if (count != 1) return G_SOURCE_REMOVE;
  void *source = ((void *(*)(void *, int))(image + UI_CAPTURE_SOURCE_RVA))(controller, 0);
  unsigned char id[48] __attribute__((aligned(8)));
  memcpy(id, source, sizeof id);
  int type; memcpy(&type, id, 4);
  if (type != c->type) return G_SOURCE_REMOVE;
  c->completed = TRUE;
  g_printerr("Gnosis capture: completing explicit portal selection\n");
  ((void (*)(void *, void *))(image + UI_CAPTURE_ACCEPT_SOURCE_RVA))(view, id);
  return G_SOURCE_REMOVE;
}
static void capture_selected(void *controller) {
  void *view = pointer_at(controller, 0x10);
  original_capture_selected(controller);
  Capture *c = find_capture(view);
  if (!c || !c->hidden || capture_controller(c) != controller) return;
  c->selected = TRUE;
  /* Portal completion and source enumeration arrive separately. Wait for both;
   * source-added will retry if enumeration has not arrived yet. */
  g_idle_add(finish_portal_capture, view);
}
static void capture_added(void *controller, int index) {
  void *view = pointer_at(controller, 0x10);
  original_capture_added(controller, index);
  Capture *c = find_capture(view);
  if (c && c->selected && c->hidden && capture_controller(c) == controller)
    g_idle_add(finish_portal_capture, view);
}
/* Keep every portal option and callback intact. Expand to the portal's combined
 * screen/window chooser only for a single unambiguous picker whose native
 * source lists permit both. Restricted/window-only requests are never widened. */
void g_dbus_proxy_call(GDBusProxy *proxy, const gchar *method, GVariant *parameters,
                      GDBusCallFlags flags, gint timeout, GCancellable *cancellable,
                      GAsyncReadyCallback callback, gpointer data) {
  typedef void (*Call)(GDBusProxy *, const gchar *, GVariant *, GDBusCallFlags,
                       gint, GCancellable *, GAsyncReadyCallback, gpointer);
  static gsize next;
  if (g_once_init_enter(&next)) {
    gpointer function = dlsym(RTLD_NEXT, "g_dbus_proxy_call");
    if (!function) _exit(127);
    g_once_init_leave(&next, (gsize)function);
  }
  if (!strcmp(method, "SelectSources") && g_atomic_int_get(&capture_total) == 1 &&
      g_atomic_int_get(&capture_combined) == 1 &&
      !g_strcmp0(g_dbus_proxy_get_interface_name(proxy), "org.freedesktop.portal.ScreenCast") &&
      parameters && g_variant_is_of_type(parameters, G_VARIANT_TYPE("(oa{sv})"))) {
    const char *handle; GVariant *options;
    g_variant_ref_sink(parameters);
    g_variant_get(parameters, "(&o@a{sv})", &handle, &options);
    GVariantDict dictionary; g_variant_dict_init(&dictionary, options);
    g_variant_dict_insert(&dictionary, "types", "u", 3u);
    GVariant *combined = g_variant_new("(o@a{sv})", handle, g_variant_dict_end(&dictionary));
    g_variant_unref(options); g_variant_unref(parameters); parameters = combined;
    g_printerr("Gnosis capture: portal offers screens and windows\n");
  }
  ((Call)next)(proxy, method, parameters, flags, timeout, cancellable, callback, data);
}
static void capture_dismissed(void *controller) {
  Capture *c = find_capture(pointer_at(controller, 0x10));
  if (c && c->hidden && capture_controller(c) == controller) {
    g_printerr("Gnosis capture: portal cancelled\n");
    native_call(UI_CAPTURE_REJECT_RVA, c->view);
    return;
  }
  original_capture_dismissed(controller);
}

static void (*original_download_begin)(void *, void *, void *);
static void download_begin(void *handler, void *frame, void *item) {
  void *table = pointer_at(item, 0);
  int source = ((int (*)(void *))pointer_at(table, 0x190))(item);
  /* DownloadSource::EXTENSION_INSTALLER. The upstream installer owns this
   * CRX until signature verification and unpacking finish. Registering it with
   * GTK moves its file too early; do not emit a user-download event or add the
   * DevTools progress observer. Ordinary CRX downloads are not filtered. */
  if (source == 5) {
    g_printerr("Gnosis extension installer: retaining native CRX ownership\n");
    return;
  }
  original_download_begin(handler, frame, item);
}

#include "native-authenticator-ui.inc"
#include "native-find.inc"
#include "native-external-ui.inc"

#include "native-close.inc"

__attribute__((constructor)) static void initialize(void) {
  /* Only the browser ELF is preloaded by our private runtime launcher. */
  void *main = dlsym(RTLD_DEFAULT, "ChromeMain");
  const char *value = g_getenv("GNOSIS_NATIVE_UI_FD");
  if (!main || !value || strcmp(value, "5")) return;
  /* The shim is already mapped into this browser. Do not inject its GTK/GLib
   * dependencies into renderer/GPU execs or desktop tools using another libc. */
  g_unsetenv("LD_PRELOAD");
  Dl_info info;
  if (!dladdr(main, &info)) return;
  image = (uintptr_t)info.dli_fbase;
  g_unsetenv("GNOSIS_NATIVE_UI_FD");
  (void)gnosis_adapter_enabled("permission-dialogs");
  g_unsetenv("GNOSIS_DISABLED_ADAPTERS");
  channel = 5;
  fcntl(channel, F_SETFD, FD_CLOEXEC);
  int type = 0; socklen_t size = sizeof type;
  if (getsockopt(channel, SOL_SOCKET, SO_TYPE, &type, &size) || type != SOCK_SEQPACKET) { channel = -1; return; }
  g_autoptr(GError) error = NULL;
  g_autoptr(GMappedFile) file = g_mapped_file_new("/proc/self/exe", FALSE, &error);
  g_autofree char *hash = file ? g_compute_checksum_for_data(G_CHECKSUM_SHA256,
    (const guchar *)g_mapped_file_get_contents(file), g_mapped_file_get_length(file)) : NULL;
  if (!hash || (strcmp(hash, UI_BINARY_SHA256) && strcmp(hash, UI_NATIVE_SCROLLBAR_BINARY_SHA256)) || !matches(UI_SHOW_TAIL_RVA, UI_SHOW_TAIL_BYTES) ||
      !matches(UI_AUTHENTICATOR_SHOW_RVA, UI_AUTHENTICATOR_SHOW_BYTES) ||
      !matches(UI_AUTHENTICATOR_CONSTRUCT_RVA, UI_AUTHENTICATOR_CONSTRUCT_BYTES) ||
      !matches(UI_AUTHENTICATOR_UPDATE_RVA, UI_AUTHENTICATOR_UPDATE_BYTES) ||
      !matches(UI_AUTHENTICATOR_DESTROY_RVA, UI_AUTHENTICATOR_DESTROY_BYTES) ||
      !matches(UI_AUTHENTICATOR_MECHANISM_METRIC_RVA, UI_AUTHENTICATOR_MECHANISM_METRIC_BYTES) ||
      !matches(UI_AUTHENTICATOR_ATTESTATION_RVA, UI_AUTHENTICATOR_ATTESTATION_BYTES) ||
      !matches(UI_REPEATING_CLOSURE_RUN_RVA, UI_REPEATING_CLOSURE_RUN_BYTES) ||
      !matches(UI_DESTROY_RVA, UI_DESTROY_BYTES) ||
      !matches(UI_PERMISSION_SHOW_RVA, UI_PERMISSION_SHOW_BYTES) ||
      !matches(UI_PERMISSION_DESTROY_RVA, UI_PERMISSION_DESTROY_BYTES) ||
      !matches(UI_RELAUNCH_RVA, UI_RELAUNCH_BYTES) ||
      !matches(UI_CHOOSER_CONSTRUCT_RVA, UI_CHOOSER_CONSTRUCT_BYTES) ||
      !matches(UI_CHOOSER_DESTROY_RVA, UI_CHOOSER_DESTROY_BYTES) ||
      !matches(UI_WIDGET_SHOW_RVA, UI_WIDGET_SHOW_BYTES) ||
      !matches(UI_FILE_DIALOG_RVA, UI_FILE_DIALOG_BYTES) ||
      !matches(UI_CREATE_MODAL_RVA, UI_CREATE_MODAL_BYTES) ||
      !matches(UI_MODEL_HOST_DESTROY_RVA, UI_MODEL_HOST_DESTROY_BYTES) ||
      !matches(UI_DIALOG_ACCEPT_RVA, UI_DIALOG_ACCEPT_BYTES) ||
      !matches(UI_DIALOG_CANCEL_RVA, UI_DIALOG_CANCEL_BYTES) ||
      !matches(UI_CAPTURE_CONSTRUCT_RVA, UI_CAPTURE_CONSTRUCT_BYTES) ||
      !matches(UI_CAPTURE_DESTROY_RVA, UI_CAPTURE_DESTROY_BYTES) ||
      !matches(UI_CAPTURE_SELECTED_RVA, UI_CAPTURE_SELECTED_BYTES) ||
      !matches(UI_CAPTURE_ADDED_RVA, UI_CAPTURE_ADDED_BYTES) ||
      !matches(UI_CAPTURE_DISMISSED_RVA, UI_CAPTURE_DISMISSED_BYTES) ||
      !matches(UI_CAPTURE_WIDGET_RVA, UI_CAPTURE_WIDGET_BYTES) ||
      !matches(UI_DOWNLOAD_BEGIN_RVA, UI_DOWNLOAD_BEGIN_BYTES) ||
      !matches(UI_EXTENSION_MODEL_SHOW_RVA, UI_EXTENSION_MODEL_SHOW_BYTES) ||
      !matches(UI_UNINSTALL_MODEL_CALL_RVA, UI_UNINSTALL_MODEL_CALL_BYTES) ||
      !matches(UI_MODEL_HOST_CONSTRUCT_RVA, UI_MODEL_HOST_CONSTRUCT_BYTES) ||
      !matches(UI_TRANSLATE_UI_RVA, UI_TRANSLATE_UI_BYTES) ||
      !matches(UI_TRANSLATE_PAGE_RVA, UI_TRANSLATE_PAGE_BYTES) ||
      !matches(UI_TRANSLATE_REVERT_RVA, UI_TRANSLATE_REVERT_BYTES) ||
      !matches(UI_TRANSLATE_SUPPORTED_RVA, UI_TRANSLATE_SUPPORTED_BYTES) ||
      !matches(UI_TRANSLATE_VISIBLE_URL_RVA, UI_TRANSLATE_VISIBLE_URL_BYTES) ||
      !matches(UI_TRANSLATE_URL_ALLOWED_RVA, UI_TRANSLATE_URL_ALLOWED_BYTES) ||
      !matches(UI_TRANSLATE_IS_TRANSLATED_RVA, UI_TRANSLATE_IS_TRANSLATED_BYTES) ||
      !matches(UI_AGENT_FOR_ID_RVA, UI_AGENT_FOR_ID_BYTES) ||
      !matches(UI_GET_USER_DATA_RVA, UI_GET_USER_DATA_BYTES)) {
    g_printerr("Gnosis native install UI: unsupported binary; keeping native dialog\n");
    close(channel); channel = -1; return;
  }
  /* Restore only the exact original instructions recorded by the binary pin.
   * The on-disk hash was checked above; no browser policy is changed. */
  for (size_t i=0;i<G_N_ELEMENTS(debug_optional_patches);++i) {
    const struct DebugOptionalPatch *patch=&debug_optional_patches[i];
    if (!gnosis_adapter_enabled(patch->id) && !replace_code(patch->offset,patch->original,patch->length)) _exit(126);
  }
  unsigned char *trampoline = mmap(NULL, 4096, PROT_READ | PROT_WRITE,
    MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
  if (trampoline == MAP_FAILED) { close(channel); channel = -1; return; }
  memcpy(trampoline, (void *)(image + UI_DESTROY_RVA), 14);
  absolute_jump(trampoline + 14, (void *)(image + UI_DESTROY_RVA + 14));
  /* Relocate the ShowWidget prologue's direct call, and the destructor's
   * RIP-relative vtable load. The original instruction spans are pinned. */
  memcpy(trampoline + 64, (void *)(image + UI_PERMISSION_SHOW_RVA), 9);
  memcpy(trampoline + 73, "\x48\xb8", 2);
  uintptr_t native_window = image + UI_PERMISSION_NATIVE_WINDOW_RVA;
  memcpy(trampoline + 75, &native_window, 8);
  memcpy(trampoline + 83, "\xff\xd0", 2);
  absolute_jump(trampoline + 85, (void *)(image + UI_PERMISSION_SHOW_RVA + 14));
  memcpy(trampoline + 128, (void *)(image + UI_PERMISSION_DESTROY_RVA), 10);
  memcpy(trampoline + 138, "\x48\xb8", 2);
  uintptr_t native_table = image + UI_PERMISSION_BASE_VTABLE_RVA;
  memcpy(trampoline + 140, &native_table, 8);
  absolute_jump(trampoline + 148, (void *)(image + UI_PERMISSION_DESTROY_RVA + 17));
  memcpy(trampoline + 192, (void *)(image + UI_CHOOSER_CONSTRUCT_RVA), 20);
  absolute_jump(trampoline + 212, (void *)(image + UI_CHOOSER_CONSTRUCT_RVA + 20));
  memcpy(trampoline + 256, (void *)(image + UI_CHOOSER_DESTROY_RVA), 23);
  absolute_jump(trampoline + 279, (void *)(image + UI_CHOOSER_DESTROY_RVA + 23));
  memcpy(trampoline + 320, (void *)(image + UI_WIDGET_SHOW_RVA), 15);
  absolute_jump(trampoline + 335, (void *)(image + UI_WIDGET_SHOW_RVA + 15));
  memcpy(trampoline + 384, (void *)(image + UI_CREATE_MODAL_RVA), 17);
  absolute_jump(trampoline + 401, (void *)(image + UI_CREATE_MODAL_RVA + 17));
  memcpy(trampoline + 448, (void *)(image + UI_MODEL_HOST_DESTROY_RVA), 10);
  memcpy(trampoline + 458, "\x48\xb8", 2);
  uintptr_t model_host_table = image + UI_MODEL_HOST_VTABLE_RVA;
  memcpy(trampoline + 460, &model_host_table, 8);
  absolute_jump(trampoline + 468, (void *)(image + UI_MODEL_HOST_DESTROY_RVA + 17));
  memcpy(trampoline + 512, (void *)(image + UI_CAPTURE_CONSTRUCT_RVA), 20);
  absolute_jump(trampoline + 532, (void *)(image + UI_CAPTURE_CONSTRUCT_RVA + 20));
  memcpy(trampoline + 576, (void *)(image + UI_CAPTURE_DESTROY_RVA), 14);
  absolute_jump(trampoline + 590, (void *)(image + UI_CAPTURE_DESTROY_RVA + 14));
  memcpy(trampoline + 640, (void *)(image + UI_CAPTURE_SELECTED_RVA), 16);
  absolute_jump(trampoline + 656, (void *)(image + UI_CAPTURE_SELECTED_RVA + 16));
  memcpy(trampoline + 704, (void *)(image + UI_CAPTURE_ADDED_RVA), 20);
  absolute_jump(trampoline + 724, (void *)(image + UI_CAPTURE_ADDED_RVA + 20));
  memcpy(trampoline + 768, (void *)(image + UI_CAPTURE_DISMISSED_RVA), 9);
  absolute_jump(trampoline + 777, (void *)(image + UI_CAPTURE_NATIVE_DISMISS_RVA));
  memcpy(trampoline + 832, (void *)(image + UI_CAPTURE_WIDGET_RVA), 20);
  absolute_jump(trampoline + 852, (void *)(image + UI_CAPTURE_WIDGET_RVA + 20));
  memcpy(trampoline + 896, (void *)(image + UI_DOWNLOAD_BEGIN_RVA), 20);
  absolute_jump(trampoline + 916, (void *)(image + UI_DOWNLOAD_BEGIN_RVA + 20));
  memcpy(trampoline + 960, (void *)(image + UI_EXTENSION_MODEL_SHOW_RVA), 16);
  absolute_jump(trampoline + 976, (void *)(image + UI_EXTENSION_MODEL_SHOW_RVA + 16));
  memcpy(trampoline + 1024, (void *)(image + UI_MODEL_HOST_CONSTRUCT_RVA), 20);
  absolute_jump(trampoline + 1044, (void *)(image + UI_MODEL_HOST_CONSTRUCT_RVA + 20));
  memcpy(trampoline + 1152, (void *)(image + UI_AUTHENTICATOR_DESTROY_RVA), 23);
  absolute_jump(trampoline + 1175, (void *)(image + UI_AUTHENTICATOR_DESTROY_RVA + 23));
  memcpy(trampoline + 1216, (void *)(image + UI_AUTHENTICATOR_UPDATE_RVA), 17);
  absolute_jump(trampoline + 1233, (void *)(image + UI_AUTHENTICATOR_UPDATE_RVA + 17));
  memcpy(trampoline + 1280, (void *)(image + UI_AUTHENTICATOR_CONSTRUCT_RVA), 19);
  absolute_jump(trampoline + 1299, (void *)(image + UI_AUTHENTICATOR_CONSTRUCT_RVA + 19));
  if (mprotect(trampoline, 4096, PROT_READ | PROT_EXEC)) { munmap(trampoline, 4096); close(channel); channel = -1; return; }
  original_destroy = (NativeVoid)trampoline;
  original_authenticator_destroy = (NativeVoid)(trampoline + 1152);
  original_authenticator_update = (NativeVoid)(trampoline + 1216);
  original_authenticator_construct = (void (*)(void *, void *, void *))(trampoline + 1280);
  original_permission_show = (NativeVoid)(trampoline + 64);
  original_permission_destroy = (NativeVoid)(trampoline + 128);
  unsigned char destructor[14]; absolute_jump(destructor, destroy_dialog);
  unsigned char show[36]; memset(show, 0x90, sizeof show);
  /* The original caller has the owned Widget in rax and the dialog View in
   * callee-saved r13. Call our adapter, then run its original epilogue. */
  memcpy(show, "\x4c\x89\xee\x48\x89\xc7\x48\xb8", 8);
  void *hook = show_install; memcpy(show + 8, &hook, sizeof hook);
  memcpy(show + 16, "\xff\xd0\x48\x83\xc4\x08\x5b\x41\x5c\x41\x5d\x41\x5e\x41\x5f\x5d\xc3", 17);
  unsigned char permission_show[14], permission_destroy[14], relaunch[14];
  absolute_jump(relaunch, request_relaunch);
  absolute_jump(permission_show, show_permission);
  absolute_jump(permission_destroy, destroy_permission);
  if (!replace_code(UI_RELAUNCH_RVA, relaunch, sizeof relaunch) ||
      !replace_code(UI_PERMISSION_SHOW_RVA, permission_show, sizeof permission_show) ||
      !replace_code(UI_PERMISSION_DESTROY_RVA, permission_destroy, sizeof permission_destroy) ||
      !replace_code(UI_DESTROY_RVA, destructor, sizeof destructor) ||
      !replace_code(UI_SHOW_TAIL_RVA, show, sizeof show)) _exit(126);
  original_chooser_construct = (void (*)(void *, void *, void *))(trampoline + 192);
  original_chooser_destroy = (NativeVoid)(trampoline + 256);
  original_widget_show = (NativeVoid)(trampoline + 320);
  unsigned char jump[14];
  absolute_jump(jump, chooser_construct);
  if (!replace_code(UI_CHOOSER_CONSTRUCT_RVA, jump, 14)) _exit(126);
  absolute_jump(jump, chooser_destroy);
  if (!replace_code(UI_CHOOSER_DESTROY_RVA, jump, 14)) _exit(126);
  absolute_jump(jump, show_widget);
  if (!replace_code(UI_WIDGET_SHOW_RVA, jump, 14)) _exit(126);
  original_create_modal = (void *(*)(void *, int, gboolean, gboolean))(trampoline + 384);
  original_model_host_destroy = (NativeVoid)(trampoline + 448);
  absolute_jump(jump, file_access_dialog);
  if (!replace_code(UI_FILE_DIALOG_RVA, jump, 14)) _exit(126);
  absolute_jump(jump, create_modal);
  if (!replace_code(UI_CREATE_MODAL_RVA, jump, 14)) _exit(126);
  absolute_jump(jump, destroy_model_host);
  if (!replace_code(UI_MODEL_HOST_DESTROY_RVA, jump, 14)) _exit(126);
  void (*callbacks[7])(void *, size_t) = {chooser_changed_0, chooser_changed_1, chooser_changed_2,
    chooser_changed_3, chooser_changed_4, chooser_changed_5, chooser_changed_6};
  uintptr_t table_page = (image + UI_CHOOSER_CALLBACKS_RVA) & ~(sysconf(_SC_PAGESIZE) - 1);
  if (mprotect((void *)table_page, sysconf(_SC_PAGESIZE), PROT_READ | PROT_WRITE)) _exit(126);
  for (unsigned i = 0; i < 7; ++i) {
    memcpy(&chooser_callbacks[i], (void *)(image + UI_CHOOSER_CALLBACKS_RVA + i * 8), 8);
    memcpy((void *)(image + UI_CHOOSER_CALLBACKS_RVA + i * 8), &callbacks[i], 8);
  }
  if (mprotect((void *)table_page, sysconf(_SC_PAGESIZE), PROT_READ)) _exit(126);
  original_capture_construct = (void (*)(void *, void *, void *, void *))(trampoline + 512);
  original_capture_destroy = (NativeVoid)(trampoline + 576);
  original_capture_widget = (void *(*)(void *, void *, void *, void *, void *))(trampoline + 832);
  absolute_jump(jump, capture_widget);
  if (!replace_code(UI_CAPTURE_WIDGET_RVA, jump, 14)) _exit(126);
  absolute_jump(jump, capture_construct);
  if (!replace_code(UI_CAPTURE_CONSTRUCT_RVA, jump, 14)) _exit(126);
  absolute_jump(jump, capture_destroy);
  if (!replace_code(UI_CAPTURE_DESTROY_RVA, jump, 14)) _exit(126);
  /* LTO devirtualizes these observer calls in the pinned browser. Hook their
   * implementations, covering both direct and virtual native callers. */
  original_capture_selected = (NativeVoid)(trampoline + 640);
  original_capture_added = (void (*)(void *, int))(trampoline + 704);
  original_capture_dismissed = (NativeVoid)(trampoline + 768);
  absolute_jump(jump, capture_selected);
  if (!replace_code(UI_CAPTURE_SELECTED_RVA, jump, 14)) _exit(126);
  absolute_jump(jump, capture_added);
  if (!replace_code(UI_CAPTURE_ADDED_RVA, jump, 14)) _exit(126);
  absolute_jump(jump, capture_dismissed);
  if (!replace_code(UI_CAPTURE_DISMISSED_RVA, jump, 14)) _exit(126);
  original_download_begin = (void (*)(void *, void *, void *))(trampoline + 896);
  absolute_jump(jump, download_begin);
  if (gnosis_adapter_enabled("downloads") && !replace_code(UI_DOWNLOAD_BEGIN_RVA, jump, 14)) _exit(126);
  original_extension_model_show = (void (*)(void *, void *, void *))(trampoline + 960);
  original_model_host_construct = (void (*)(void *, void *, void *, int, int, int, int))(trampoline + 1024);
  absolute_jump(jump, show_extension_model);
  if (!replace_code(UI_EXTENSION_MODEL_SHOW_RVA, jump, 14)) _exit(126);
  absolute_jump(jump, model_host_construct);
  if (!replace_code(UI_MODEL_HOST_CONSTRUCT_RVA, jump, 14)) _exit(126);
  absolute_jump(jump, translate_ui);
  if (gnosis_adapter_enabled("translation") && !replace_code(UI_TRANSLATE_UI_RVA, jump, 14)) _exit(126);
  absolute_jump(jump, authenticator_construct);
  if (!replace_code(UI_AUTHENTICATOR_CONSTRUCT_RVA, jump, 14)) _exit(126);
  absolute_jump(jump, authenticator_destroy);
  if (!replace_code(UI_AUTHENTICATOR_DESTROY_RVA, jump, 14)) _exit(126);
  absolute_jump(jump, authenticator_update);
  if (!replace_code(UI_AUTHENTICATOR_UPDATE_RVA, jump, 14)) _exit(126);
  absolute_jump(jump, authenticator_show);
  if (!replace_code(UI_AUTHENTICATOR_SHOW_RVA, jump, 14)) _exit(126);
  if (!input_watch) input_watch = g_unix_fd_add(channel, G_IO_IN | G_IO_HUP | G_IO_ERR, receive_response, NULL);
  if (!native_external_install()) { g_printerr("Gnosis external applications: unsupported binary; keeping native dialog\n"); }
  if (!native_close_install()) { g_printerr("Gnosis close checks: unsupported binary\n"); close(channel); channel = -1; return; }
  if (!native_find_install()) g_printerr("Gnosis native find: unsupported binary; find disabled\n");
  g_printerr("Gnosis native install UI: exact-binary presentation hook ready\n");
}
