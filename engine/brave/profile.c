/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "profile.h"
#include <glib/gstdio.h>
#include <json-glib/json-glib.h>
#include <errno.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

static JsonObject *object_member(JsonObject *parent, const char *name, GError **error) {
  if (!json_object_has_member(parent, name)) {
    JsonObject *child = json_object_new(); json_object_set_object_member(parent, name, child); return child;
  }
  JsonNode *node = json_object_get_member(parent, name);
  if (JSON_NODE_HOLDS_OBJECT(node)) return json_node_get_object(node);
  g_set_error(error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA, "Brave Local State member %s is not an object", name);
  return NULL;
}
/* Disable automatic native translation offers. This leaves explicit translate
 * commands available for the GTK controller, and preserves other prefs. */
static gboolean prepare_translation_preferences(const char *directory, GError **error) {
  g_autofree char *profile = g_build_filename(directory, "Default", NULL);
  if (g_mkdir_with_parents(profile, 0700) < 0) {
    g_set_error(error, G_IO_ERROR, g_io_error_from_errno(errno), "Cannot create Brave profile"); return FALSE;
  }
  g_autofree char *path = g_build_filename(profile, "Preferences", NULL);
  g_autoptr(JsonParser) parser = json_parser_new();
  g_autoptr(JsonNode) root = NULL;
  if (g_file_test(path, G_FILE_TEST_EXISTS)) {
    if (!json_parser_load_from_file(parser, path, error)) return FALSE;
    root = json_node_copy(json_parser_get_root(parser));
  } else {
    root = json_node_new(JSON_NODE_OBJECT); json_node_take_object(root, json_object_new());
  }
  if (!JSON_NODE_HOLDS_OBJECT(root)) {
    g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA, "Brave Preferences is not a JSON object"); return FALSE;
  }
  /* The GTK shell owns restoration; never restore hidden Chromium windows. */
  JsonObject *session = object_member(json_node_get_object(root), "session", error);
  if (!session) return FALSE;
  json_object_set_int_member(session, "restore_on_startup", 5); /* Chromium NEW_TAB */
  JsonObject *translate = object_member(json_node_get_object(root), "translate", error);
  if (!translate) return FALSE;
  json_object_set_boolean_member(translate, "enabled", FALSE);
  /* KeePassXC owns Gnosis's credential UI. Keep existing native vault data,
   * but stop Brave from offering a second password store after form submits. */
  json_object_set_boolean_member(json_node_get_object(root), "credentials_enable_service", FALSE);
  json_object_set_boolean_member(json_node_get_object(root), "credentials_enable_autosignin", FALSE);
  // The bundled full uBlock Origin and validated local installations are real
  // unpacked extensions. Chromium otherwise disables them on self-reload with
  // DISABLE_UNSUPPORTED_DEVELOPER_EXTENSION, even after loadUnpacked succeeds.
  JsonObject *extensions = object_member(json_node_get_object(root), "extensions", error);
  JsonObject *ui = extensions ? object_member(extensions, "ui", error) : NULL;
  if (!ui) return FALSE;
  json_object_set_boolean_member(ui, "developer_mode", TRUE);
  g_autofree char *json = json_to_string(root, TRUE);
  return g_file_set_contents_full(path, json, -1, G_FILE_SET_CONTENTS_CONSISTENT, 0600, error);
}
gboolean gnosis_brave_profile_prepare(const char *directory, GError **error) {
  if (g_mkdir_with_parents(directory, 0700) < 0) {
    g_set_error(error, G_IO_ERROR, g_io_error_from_errno(errno), "Cannot create Gnosis's Brave profile directory"); return FALSE;
  }
  char *path = g_build_filename(directory, "Local State", NULL);
  JsonParser *parser = json_parser_new(); JsonNode *root = NULL;
  if (g_file_test(path, G_FILE_TEST_EXISTS)) {
    if (!json_parser_load_from_file(parser, path, error)) { g_object_unref(parser); g_free(path); return FALSE; }
    root = json_node_copy(json_parser_get_root(parser));
  } else {
    root = json_node_new(JSON_NODE_OBJECT); json_node_take_object(root, json_object_new());
  }
  g_object_unref(parser);
  if (!JSON_NODE_HOLDS_OBJECT(root)) {
    g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA, "Brave Local State is not a JSON object");
    json_node_free(root); g_free(path); return FALSE;
  }
  JsonObject *brave = object_member(json_node_get_object(root), "brave", error);
  JsonObject *p3a = brave ? object_member(brave, "p3a", error) : NULL;
  if (!p3a) { json_node_free(root); g_free(path); return FALSE; }
  // Brave v1.97.56's BraveConfirmP3AInfoBarDelegate checks these actual local-
  // state preferences before creating its analytics infobar. Gnosis owns its
  // UI and does not enable Brave's product analytics in its isolated profiles.
  json_object_set_boolean_member(p3a, "enabled", FALSE);
  json_object_set_boolean_member(p3a, "notice_acknowledged", TRUE);
  char *json = json_to_string(root, TRUE);
  gboolean result = g_file_set_contents_full(path, json, -1, G_FILE_SET_CONTENTS_CONSISTENT, 0600, error);
  g_free(json); json_node_free(root); g_free(path); return result && prepare_translation_preferences(directory, error);
}


char *gnosis_brave_password_manager(const char *directory) {
  g_autofree char *path = g_build_filename(directory, "password-manager", NULL);
  char *value = NULL;
  if (g_file_get_contents(path, &value, NULL, NULL) &&
      (g_str_equal(value, "keyslot") || g_str_equal(value, "keepassxc"))) return value;
  g_free(value); return g_strdup("auto");
}

static char *keyslot_proxy(void) {
  /* Keyslot's browser setup also works when its trial install is outside PATH. */
  g_autofree char *path = g_build_filename(g_get_user_config_dir(), "gnosis", "NativeMessagingHosts",
    "org.keepassxc.keepassxc_browser.json", NULL);
  g_autoptr(JsonParser) parser = json_parser_new();
  if (json_parser_load_from_file(parser, path, NULL)) {
    JsonNode *root = json_parser_get_root(parser);
    if (JSON_NODE_HOLDS_OBJECT(root)) {
      JsonObject *host = json_node_get_object(root);
      JsonNode *node = json_object_get_member(host, "path");
      JsonNode *description = json_object_get_member(host, "description");
      if (node && JSON_NODE_HOLDS_VALUE(node) && json_node_get_value_type(node) == G_TYPE_STRING &&
          description && JSON_NODE_HOLDS_VALUE(description) && json_node_get_value_type(description) == G_TYPE_STRING &&
          g_str_equal(json_node_get_string(description), "Keyslot password manager")) {
        const char *proxy = json_node_get_string(node);
        if (g_path_is_absolute(proxy) && g_file_test(proxy, G_FILE_TEST_IS_EXECUTABLE) &&
            g_file_test(proxy, G_FILE_TEST_IS_REGULAR)) return g_strdup(proxy);
      }
    }
  }
  return g_find_program_in_path("keyslot-browser-proxy");
}

static gboolean prepare_password_manager(const char *directory, const char *manager, GError **error) {
  g_autofree char *proxy = NULL;
  if (!g_str_equal(manager, "keepassxc")) proxy = keyslot_proxy();
  if (!proxy && !g_str_equal(manager, "keyslot")) proxy = g_find_program_in_path("keepassxc-proxy");
  if (!proxy) {
    g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_NOT_FOUND,
      g_str_equal(manager, "keyslot") ? "Enable Gnosis in Keyslot’s Browser Setup first" : "Install a password manager to connect browser integration");
    return FALSE;
  }
  g_autofree char *hosts = g_build_filename(directory, "native-config", "BraveSoftware", "Brave-Browser", "NativeMessagingHosts", NULL);
  if (g_mkdir_with_parents(hosts, 0700) < 0) {
    g_set_error_literal(error, G_IO_ERROR, g_io_error_from_errno(errno), "Cannot create Gnosis's native messaging directory");
    return FALSE;
  }
  /* Native hosts use their own installed runtime, not Brave's Nix library
   * search path. Restore the desktop config directory before executing the
   * real proxy, so KeePassXC keeps using its own settings and database. */
  g_autofree char *launcher = g_build_filename(hosts, "gnosis-keepassxc-proxy", NULL);
  g_autofree char *quoted_proxy = g_shell_quote(proxy);
  g_autofree char *quoted_config = g_shell_quote(g_get_user_config_dir());
  g_autofree char *script = g_strdup_printf(
    "#!/bin/sh\nunset LD_LIBRARY_PATH LD_PRELOAD GIO_EXTRA_MODULES GTK_PATH QT_PLUGIN_PATH QML2_IMPORT_PATH\n"
    "export XDG_CONFIG_HOME=%s\nexec %s \"$@\"\n", quoted_config, quoted_proxy);
  if (!g_file_set_contents_full(launcher, script, -1, G_FILE_SET_CONTENTS_CONSISTENT, 0700, error)) return FALSE;
  if (g_chmod(launcher, 0700) < 0) {
    g_set_error_literal(error, G_IO_ERROR, g_io_error_from_errno(errno), "Cannot prepare the KeePassXC proxy launcher");
    return FALSE;
  }
  g_autoptr(JsonObject) host = json_object_new();
  json_object_set_string_member(host, "name", "org.keepassxc.keepassxc_browser");
  json_object_set_string_member(host, "description", "Gnosis KeePassXC integration");
  json_object_set_string_member(host, "type", "stdio");
  json_object_set_string_member(host, "path", launcher);
  JsonArray *origins = json_array_new();
  json_array_add_string_element(origins, "chrome-extension://giohadoodmdcadnkcpdmhmmdnaidnckm/");
  json_object_set_array_member(host, "allowed_origins", origins);
  g_autoptr(JsonNode) root = json_node_new(JSON_NODE_OBJECT);
  json_node_set_object(root, host);
  g_autofree char *json = json_to_string(root, TRUE);
  g_autofree char *path = g_build_filename(hosts, "org.keepassxc.keepassxc_browser.json", NULL);
  return g_file_set_contents_full(path, json, -1, G_FILE_SET_CONTENTS_CONSISTENT, 0600, error);
}

gboolean gnosis_brave_profile_prepare_keepassxc(const char *directory, GError **error) {
  g_autofree char *manager = gnosis_brave_password_manager(directory);
  return prepare_password_manager(directory, manager, error);
}

gboolean gnosis_brave_password_manager_set(const char *directory, const char *manager, GError **error) {
  if (!g_str_equal(manager, "auto") && !g_str_equal(manager, "keyslot") && !g_str_equal(manager, "keepassxc")) {
    g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT, "Choose a supported password manager"); return FALSE;
  }
  g_autofree char *previous = gnosis_brave_password_manager(directory);
  if (!prepare_password_manager(directory, manager, error)) return FALSE;
  g_autofree char *path = g_build_filename(directory, "password-manager", NULL);
  if (g_file_set_contents_full(path, manager, -1, G_FILE_SET_CONTENTS_CONSISTENT, 0600, error)) return TRUE;
  prepare_password_manager(directory, previous, NULL);
  return FALSE;
}

#define PAIRING_LIMIT (1024 * 1024)
static gboolean valid_pairing_data(const char *data, gsize length, GError **error) {
  g_autoptr(JsonParser) parser = json_parser_new();
  gboolean valid = length <= PAIRING_LIMIT && g_utf8_validate(data, length, NULL) &&
    json_parser_load_from_data(parser, data, length, NULL) &&
    JSON_NODE_HOLDS_OBJECT(json_parser_get_root(parser));
  if (valid) {
    JsonObject *ring = json_node_get_object(json_parser_get_root(parser));
    g_autoptr(GList) hashes = json_object_get_members(ring);
    valid = json_object_get_size(ring) <= 256;
    for (GList *item = hashes; valid && item; item = item->next) {
      const char *hash = item->data;
      JsonNode *node = json_object_get_member(ring, hash);
      valid = strlen(hash) >= 32 && strlen(hash) <= 128 && JSON_NODE_HOLDS_OBJECT(node);
      for (const char *p = hash; valid && *p; p++) valid = g_ascii_isxdigit(*p);
      if (!valid) break;
      JsonObject *entry = json_node_get_object(node);
      valid = json_object_get_size(entry) == 3;
      const char *values[3] = {NULL, NULL, NULL};
      const char *names[3] = {"hash", "id", "key"};
      for (guint i = 0; valid && i < G_N_ELEMENTS(names); i++) {
        node = json_object_get_member(entry, names[i]);
        valid = node && JSON_NODE_HOLDS_VALUE(node) && json_node_get_value_type(node) == G_TYPE_STRING;
        if (valid) values[i] = json_node_get_string(node);
      }
      if (!valid) break;
      valid = !strcmp(values[0], hash) && *values[1] && strlen(values[1]) <= 4096 && strlen(values[2]) == 44;
      if (!valid) break;
      gsize size = 0;
      g_autofree guchar *key = g_base64_decode(values[2], &size);
      g_autofree char *encoded = g_base64_encode(key, size);
      valid = size == 32 && !strcmp(encoded, values[2]);
    }
  }
  if (!valid) g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA, "Invalid KeePassXC association data");
  return valid;
}

char *gnosis_brave_keepassxc_pairing_load(const char *path, GError **error) {
  int fd = open(path, O_RDONLY | O_CLOEXEC | O_NOFOLLOW | O_NONBLOCK);
  if (fd < 0) {
    if (errno == ENOENT) return g_strdup("{}");
    g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_FAILED, "Cannot read shared KeePassXC associations");
    return NULL;
  }
  struct stat info;
  if (fstat(fd, &info) || !S_ISREG(info.st_mode) || info.st_uid != getuid() || (info.st_mode & 077) || info.st_size > PAIRING_LIMIT) {
    close(fd);
    g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_PERMISSION_DENIED, "Unsafe shared KeePassXC association file");
    return NULL;
  }
  g_autofree char *data = g_malloc(PAIRING_LIMIT + 2);
  gsize length = 0;
  while (length <= PAIRING_LIMIT) {
    ssize_t size = read(fd, data + length, PAIRING_LIMIT + 1 - length);
    if (size < 0 && errno == EINTR) continue;
    if (size < 0) {
      close(fd);
      g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_FAILED, "Cannot read shared KeePassXC associations");
      return NULL;
    }
    if (!size) break;
    length += size;
  }
  close(fd);
  data[length] = 0;
  if (!valid_pairing_data(data, length, error)) return NULL;
  return g_steal_pointer(&data);
}

gboolean gnosis_brave_keepassxc_pairing_save(const char *path, const char *json, GError **error) {
  if (!valid_pairing_data(json, strlen(json), error)) return FALSE;
  g_autofree char *directory = g_path_get_dirname(path);
  struct stat info;
  if (g_mkdir_with_parents(directory, 0700) || stat(directory, &info) ||
      !S_ISDIR(info.st_mode) || info.st_uid != getuid() || (info.st_mode & 022)) {
    g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_PERMISSION_DENIED, "Unsafe KeePassXC association directory");
    return FALSE;
  }
  if (!lstat(path, &info)) {
    if (!S_ISREG(info.st_mode) || info.st_uid != getuid()) {
      g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_PERMISSION_DENIED, "Unsafe KeePassXC association file");
      return FALSE;
    }
  } else if (errno != ENOENT) {
    g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_FAILED, "Cannot inspect KeePassXC association file");
    return FALSE;
  }
  if (!g_file_set_contents_full(path, json, -1, G_FILE_SET_CONTENTS_CONSISTENT | G_FILE_SET_CONTENTS_DURABLE, 0600, error)) return FALSE;
  if (g_chmod(path, 0600)) {
    g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_PERMISSION_DENIED, "Cannot protect KeePassXC associations");
    return FALSE;
  }
  return TRUE;
}
