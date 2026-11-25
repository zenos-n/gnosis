/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "brave/profile.h"
#include <glib/gstdio.h>
#include <json-glib/json-glib.h>
int main(void) {
  g_autoptr(GError) error = NULL;
  g_autofree char *directory = g_dir_make_tmp("gnosis-session-prefs-XXXXXX", &error);
  g_assert_no_error(error);
  g_autofree char *profile = g_build_filename(directory, "Default", NULL);
  g_autofree char *path = g_build_filename(profile, "Preferences", NULL);
  g_assert_cmpint(g_mkdir_with_parents(profile, 0700), ==, 0);
  const char *original = "{\"session\":{\"restore_on_startup\":1,\"startup_urls\":[\"https://fixture.invalid/\"]},\"fixture\":{\"credential-marker\":\"preserve\"}}";
  g_assert_true(g_file_set_contents(path, original, -1, &error));g_assert_no_error(error);
  g_assert_true(gnosis_brave_profile_prepare(directory, &error));g_assert_no_error(error);
  g_autoptr(JsonParser) parser = json_parser_new();
  g_assert_true(json_parser_load_from_file(parser, path, &error));g_assert_no_error(error);
  JsonObject *root = json_node_get_object(json_parser_get_root(parser));
  JsonObject *session = json_object_get_object_member(root, "session");
  g_assert_cmpint(json_object_get_int_member(session, "restore_on_startup"), ==, 5);
  g_assert_cmpstr(json_array_get_string_element(json_object_get_array_member(session, "startup_urls"), 0), ==, "https://fixture.invalid/");
  g_assert_cmpstr(json_object_get_string_member(json_object_get_object_member(root, "fixture"), "credential-marker"), ==, "preserve");
  g_assert_true(gnosis_brave_profile_prepare(directory, &error));g_assert_no_error(error);
  const char *invalid = "{\"session\":[],\"fixture\":\"do not overwrite\"}";
  g_assert_true(g_file_set_contents(path, invalid, -1, &error));g_assert_no_error(error);
  g_assert_false(gnosis_brave_profile_prepare(directory, &error));g_assert_error(error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA);g_clear_error(&error);
  g_autofree char *unchanged = NULL;g_assert_true(g_file_get_contents(path, &unchanged, NULL, &error));g_assert_no_error(error);g_assert_cmpstr(unchanged, ==, invalid);
  g_unlink(path);g_rmdir(profile);g_autofree char *state = g_build_filename(directory, "Local State", NULL);g_unlink(state);g_rmdir(directory);
  g_print("BRAVE_SESSION_PREFS_PASS backend restore disabled; unrelated preferences preserved; malformed data rejected\n");return 0;
}
