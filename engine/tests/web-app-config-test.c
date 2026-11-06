/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "web-app-config.h"
#include <glib/gstdio.h>
int main(void) {
  g_autofree char *first = g_dir_make_tmp("gnosis-webapp-config-a-XXXXXX", NULL);
  g_autofree char *second = g_dir_make_tmp("gnosis-webapp-config-b-XXXXXX", NULL);
  g_autoptr(GKeyFile) config = gnosis_web_app_config_load(first, NULL);
  g_autoptr(GKeyFile) other = NULL;
  g_autoptr(GError) error = NULL;
  g_autoptr(GKeyFile) links = g_key_file_new();
  const char *domains[] = {"x.com", "twitter.com", "bücher.example"};
  const char *enabled[] = {"keepassxc", "abcdefghijklmnopabcdefghijklmnop"};
  g_autofree char *path = g_build_filename(first, GNOSIS_WEB_APP_CONFIG, NULL);
  g_autofree char *second_path = g_build_filename(second, GNOSIS_WEB_APP_CONFIG, NULL);
  g_assert_nonnull(config);
  g_assert_false(g_key_file_has_group(config, "Extensions"));
  g_key_file_set_string_list(config, "Extensions", "enabled", enabled, 2);
  g_key_file_set_string(config, enabled[1], "path", "/tmp/installed-extension");
  g_assert_true(gnosis_web_app_config_save(first, config, &error));
  g_assert_no_error(error);
  other = gnosis_web_app_config_load(first, &error);
  g_assert_no_error(error);
  g_assert_true(gnosis_web_app_extension_enabled(other, "keepassxc"));
  g_assert_false(gnosis_web_app_extension_enabled(other, "ublock"));
  g_key_file_set_string_list(other, "Extensions", "enabled", NULL, 0);
  g_assert_true(gnosis_web_app_config_save(second, other, &error));
  g_clear_pointer(&other, g_key_file_unref);
  other = gnosis_web_app_config_load(second, &error);
  g_assert_true(g_key_file_has_group(other, "Extensions"));
  g_assert_false(gnosis_web_app_extension_enabled(other, "keepassxc"));
  g_key_file_set_string(config, enabled[1], "path", "relative/path");
  g_assert_false(gnosis_web_app_config_save(first, config, &error));
  g_assert_error(error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA);
  g_clear_error(&error);
  g_clear_pointer(&other, g_key_file_unref);
  other = gnosis_web_app_config_load(first, &error);
  g_assert_true(gnosis_web_app_extension_enabled(other, "keepassxc"));
  g_assert_false(gnosis_web_app_extension_id_valid("../../outside"));
  g_file_set_contents(path, "[Extensions]\nenabled=../../outside;\n", -1, NULL);
  g_clear_pointer(&other, g_key_file_unref);
  other = gnosis_web_app_config_load(first, &error);
  g_assert_null(other);
  g_assert_error(error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA);
  g_clear_error(&error);
  g_key_file_set_boolean(links,"Links","enabled",TRUE);
  g_key_file_set_string_list(links,"Links","domains",domains,3);
  g_assert_true(gnosis_web_app_config_validate(links,&error));
  g_assert_no_error(error);
  g_assert_cmpuint(gnosis_web_app_link_match(links,"https://x.com/a?url=https://evil.test"),==,5);
  g_assert_cmpuint(gnosis_web_app_link_match(links,"http://MOBILE.X.COM/a"),==,5);
  g_assert_cmpuint(gnosis_web_app_link_match(links,"https://twitter.com/a"),==,11);
  g_assert_cmpuint(gnosis_web_app_link_match(links,"https://xn--bcher-kva.example/a"),>,0);
  g_assert_cmpuint(gnosis_web_app_link_match(links,"https://notx.com"),==,0);
  g_assert_cmpuint(gnosis_web_app_link_match(links,"https://x.com.evil.test"),==,0);
  g_assert_cmpuint(gnosis_web_app_link_match(links,"https://x.com@evil.test"),==,0);
  g_assert_cmpuint(gnosis_web_app_link_match(links,"javascript:location='https://x.com'"),==,0);
  g_assert_cmpuint(gnosis_web_app_link_match(links,"file:///x.com"),==,0);
  g_assert_cmpuint(gnosis_web_app_link_match(links,"not a URI"),==,0);
  g_key_file_set_boolean(links,"Links","enabled",FALSE);
  g_assert_cmpuint(gnosis_web_app_link_match(links,"https://x.com/a"),==,0);
  g_assert_true(gnosis_web_app_config_save(second,links,&error));
  g_clear_pointer(&other,g_key_file_unref);
  other=gnosis_web_app_config_load(second,&error);
  g_assert_no_error(error);
  g_assert_false(g_key_file_get_boolean(other,"Links","enabled",NULL));
  for (const char **domain=(const char *[]){"*.x.com","https://x.com","x.com/path","-x.com","x..com","x.com:443",NULL};*domain;domain++) {
    g_key_file_set_string_list(links,"Links","domains",domain,1);
    g_assert_false(gnosis_web_app_config_save(second,links,&error));
    g_assert_error(error,G_IO_ERROR,G_IO_ERROR_INVALID_DATA);g_clear_error(&error);
  }
  g_print("WEB_APP_LINKS_PASS boundaries, aliases, Unicode, disabled associations, invalid config rejected\n");
  g_unlink(path); g_unlink(second_path); g_rmdir(first); g_rmdir(second);
  g_print("WEB_APP_CONFIG_PASS isolated profiles, explicit no-extensions, invalid selection rejected without overwriting\n");
}
