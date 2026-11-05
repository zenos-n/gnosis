/* -*- Mode: C; tab-width: 2; indent-tabs-mode: nil; c-basic-offset: 2; -*- */
/*
 *  Copyright © 2008 Xan Lopez <xan@gnome.org>
 *
 *  This file is part of Epiphany.
 *
 *  Epiphany is free software: you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation, either version 3 of the License, or
 *  (at your option) any later version.
 *
 *  Epiphany is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *  GNU General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License
 *  along with Epiphany.  If not, see <http://www.gnu.org/licenses/>.
 */

#include <config.h>
#include "ephy-embed-prefs.h"

#include "ephy-embed-shell.h"
#include "ephy-embed-utils.h"
#include "ephy-file-helpers.h"
#include "ephy-langs.h"
#include "ephy-prefs.h"
#include "ephy-settings.h"
#include <math.h>

typedef struct {
  const char *schema;
  const char *key;
  const char *engine_pref;
  void (*callback)(GSettings  *settings,
                   const char *key,
                   gpointer    data);
} PrefData;

#define DEFAULT_ENCODING_SETTING "default-charset"
/* FIXME: Refactor this code to remove the need of those globals */
static EphyEngineSettings *engine_settings = NULL;
static GFileMonitor *user_style_sheet_monitor = NULL;
static WebKitUserStyleSheet *style_sheet = NULL;
static GFileMonitor *user_javascript_monitor = NULL;
static WebKitUserScript *javascript = NULL;
static char *user_style_source = NULL;
static char *user_script_source = NULL;
static GList *ucm_list = NULL;

static void
update_user_style_on_all_ucm (void)
{
  GList *list = NULL;

  ephy_engine_set_user_stylesheet (style_sheet ? user_style_source : NULL);

  for (list = ucm_list; list; list = list->next) {
    WebKitUserContentManager *ucm = list->data;

    webkit_user_content_manager_remove_all_style_sheets (ucm);
    if (style_sheet)
      webkit_user_content_manager_add_style_sheet (ucm, style_sheet);
  }
}

static void
user_style_sheet_output_stream_splice_cb (GOutputStream *output_stream,
                                          GAsyncResult  *result,
                                          gpointer       user_data)
{
  gssize bytes;

  g_clear_pointer (&style_sheet, webkit_user_style_sheet_unref);

  bytes = g_output_stream_splice_finish (output_stream, result, NULL);
  g_clear_pointer (&user_style_source, g_free);
  if (bytes > 0 && g_settings_get_boolean (EPHY_SETTINGS_WEB, EPHY_PREFS_WEB_ENABLE_USER_CSS)) {
    user_style_source = g_strndup (g_memory_output_stream_get_data (G_MEMORY_OUTPUT_STREAM (output_stream)), bytes);
    style_sheet = webkit_user_style_sheet_new (user_style_source,
                                               WEBKIT_USER_CONTENT_INJECT_ALL_FRAMES, WEBKIT_USER_STYLE_LEVEL_USER,
                                               NULL, NULL);
  }

  update_user_style_on_all_ucm ();

  g_object_unref (output_stream);
}

static void
user_style_sheet_read_cb (GFile        *file,
                          GAsyncResult *result,
                          gpointer      user_data)
{
  g_autoptr (GFileInputStream) input_stream = NULL;
  g_autoptr (GOutputStream) output_stream = NULL;

  input_stream = g_file_read_finish (file, result, NULL);
  if (!input_stream)
    return;

  output_stream = g_memory_output_stream_new_resizable ();
  g_output_stream_splice_async (g_steal_pointer (&output_stream),
                                G_INPUT_STREAM (input_stream),
                                G_OUTPUT_STREAM_SPLICE_CLOSE_SOURCE | G_OUTPUT_STREAM_SPLICE_CLOSE_TARGET,
                                G_PRIORITY_DEFAULT,
                                NULL,
                                (GAsyncReadyCallback)user_style_sheet_output_stream_splice_cb,
                                NULL);
}

static void
user_style_sheet_file_changed (GFileMonitor      *monitor,
                               GFile             *file,
                               GFile             *other_file,
                               GFileMonitorEvent  event_type,
                               gpointer           user_data)
{
  if (event_type == G_FILE_MONITOR_EVENT_CHANGES_DONE_HINT) {
    g_file_read_async (file, G_PRIORITY_DEFAULT, NULL,
                       (GAsyncReadyCallback)user_style_sheet_read_cb, NULL);
  }
}

static void
engine_pref_callback_user_stylesheet (GSettings  *settings,
                                      const char *key,
                                      gpointer    data)
{
  g_autoptr (GFile) file = NULL;
  g_autofree char *filename = NULL;
  gboolean value;
  GError *error = NULL;

  value = g_settings_get_boolean (settings, key);

  if (user_style_sheet_monitor) {
    g_signal_handlers_disconnect_by_func (user_style_sheet_monitor, user_style_sheet_file_changed, NULL);
    g_clear_object (&user_style_sheet_monitor);
  }

  g_clear_pointer (&style_sheet, webkit_user_style_sheet_unref);

  if (!value) {
    update_user_style_on_all_ucm ();
    return;
  }

  filename = g_build_filename (ephy_profile_dir (), USER_STYLESHEET_FILENAME, NULL);
  file = g_file_new_for_path (filename);

  g_file_read_async (file, G_PRIORITY_DEFAULT, NULL,
                     (GAsyncReadyCallback)user_style_sheet_read_cb, NULL);

  user_style_sheet_monitor = g_file_monitor_file (file, G_FILE_MONITOR_NONE, NULL, &error);
  if (!user_style_sheet_monitor) {
    g_warning ("Could not create a file monitor for %s: %s", g_file_get_uri (file), error->message);
    g_error_free (error);
  } else {
    g_signal_connect (user_style_sheet_monitor, "changed", G_CALLBACK (user_style_sheet_file_changed), NULL);
  }
}

static void
update_user_javascript_on_all_ucm (void)
{
  GList *list = NULL;

  ephy_engine_set_user_javascript (javascript ? user_script_source : NULL);

  for (list = ucm_list; list; list = list->next) {
    WebKitUserContentManager *ucm = list->data;

    webkit_user_content_manager_remove_all_scripts (ucm);
    if (javascript)
      webkit_user_content_manager_add_script (ucm, javascript);
  }
}

static void
user_javascript_output_stream_splice_cb (GOutputStream *output_stream,
                                         GAsyncResult  *result,
                                         gpointer       user_data)
{
  gssize bytes;

  g_clear_pointer (&javascript, webkit_user_script_unref);

  bytes = g_output_stream_splice_finish (output_stream, result, NULL);
  g_clear_pointer (&user_script_source, g_free);
  if (bytes > 0 && g_settings_get_boolean (EPHY_SETTINGS_WEB, EPHY_PREFS_WEB_ENABLE_USER_JS)) {
    user_script_source = g_strndup (g_memory_output_stream_get_data (G_MEMORY_OUTPUT_STREAM (output_stream)), bytes);
    javascript = webkit_user_script_new (user_script_source,
                                         WEBKIT_USER_CONTENT_INJECT_ALL_FRAMES, WEBKIT_USER_SCRIPT_INJECT_AT_DOCUMENT_END,
                                         NULL, NULL);
  }

  update_user_javascript_on_all_ucm ();

  g_object_unref (output_stream);
}

static void
user_javascript_read_cb (GFile        *file,
                         GAsyncResult *result,
                         gpointer      user_data)
{
  g_autoptr (GFileInputStream) input_stream = NULL;
  g_autoptr (GOutputStream) output_stream = NULL;

  input_stream = g_file_read_finish (file, result, NULL);
  if (!input_stream)
    return;

  output_stream = g_memory_output_stream_new_resizable ();
  g_output_stream_splice_async (g_steal_pointer (&output_stream),
                                G_INPUT_STREAM (input_stream),
                                G_OUTPUT_STREAM_SPLICE_CLOSE_SOURCE | G_OUTPUT_STREAM_SPLICE_CLOSE_TARGET,
                                G_PRIORITY_DEFAULT,
                                NULL,
                                (GAsyncReadyCallback)user_javascript_output_stream_splice_cb,
                                NULL);
}

static void
user_javascript_file_changed (GFileMonitor      *monitor,
                              GFile             *file,
                              GFile             *other_file,
                              GFileMonitorEvent  event_type,
                              gpointer           user_data)
{
  if (event_type == G_FILE_MONITOR_EVENT_CHANGES_DONE_HINT) {
    g_file_read_async (file, G_PRIORITY_DEFAULT, NULL,
                       (GAsyncReadyCallback)user_javascript_read_cb, NULL);
  }
}

static void
engine_pref_callback_user_javascript (GSettings  *settings,
                                      const char *key,
                                      gpointer    data)
{
  g_autoptr (GFile) file = NULL;
  g_autofree char *filename = NULL;
  gboolean value;
  g_autoptr (GError) error = NULL;

  value = g_settings_get_boolean (settings, key);

  if (user_javascript_monitor) {
    g_signal_handlers_disconnect_by_func (user_javascript_monitor, user_javascript_file_changed, NULL);
    g_clear_object (&user_javascript_monitor);
  }

  g_clear_pointer (&javascript, webkit_user_script_unref);

  if (!value) {
    update_user_javascript_on_all_ucm ();
    return;
  }

  filename = g_build_filename (ephy_profile_dir (), USER_JAVASCRIPT_FILENAME, NULL);
  file = g_file_new_for_path (filename);

  g_file_read_async (file, G_PRIORITY_DEFAULT, NULL,
                     (GAsyncReadyCallback)user_javascript_read_cb, NULL);

  user_javascript_monitor = g_file_monitor_file (file, G_FILE_MONITOR_NONE, NULL, &error);
  if (!user_javascript_monitor)
    g_warning ("Could not create a file monitor for %s: %s", g_file_get_uri (file), error->message);
  else
    g_signal_connect (user_javascript_monitor, "changed", G_CALLBACK (user_javascript_file_changed), NULL);
}

static void
engine_pref_callback_user_agent (GSettings  *settings,
                                 const char *key,
                                 gpointer    data)
{
  g_autofree char *agent = g_settings_get_string (settings, key);
  g_object_set (engine_settings, "user-agent", agent, NULL);
}

static void
engine_pref_callback_font_size (GSettings  *settings,
                                const char *key,
                                gpointer    data)
{
  char *engine_pref = data;
  char *value = NULL;
  double size = 12 * 96.0 / 72.0;

  value = g_settings_get_string (settings, key);

  if (value) {
    PangoFontDescription *desc;

    desc = pango_font_description_from_string (value);
    size = pango_units_to_double (pango_font_description_get_size (desc));
    if (!pango_font_description_get_size_is_absolute (desc))
      size *= 96.0 / 72.0;
    pango_font_description_free (desc);
  }

  g_object_set (engine_settings, engine_pref, (guint)CLAMP (lround (size), 0, G_MAXINT), NULL);
  g_free (value);
}

static void
engine_pref_callback_font_family (GSettings  *settings,
                                  const char *key,
                                  gpointer    data)
{
  char *engine_pref = data;
  char *value = NULL;

  value = g_settings_get_string (settings, key);

  if (value) {
    PangoFontDescription *desc;
    const char *family = NULL;

    desc = pango_font_description_from_string (value);
    family = pango_font_description_get_family (desc);
    g_object_set (engine_settings, engine_pref, family, NULL);
    pango_font_description_free (desc);
  }

  g_free (value);
}

/* Based on Christian Persch's code from gecko backend of epiphany
 *  (old transform_accept_languages_list() function) */
static void
engine_pref_callback_accept_languages (GSettings  *settings,
                                       const char *key,
                                       gpointer    data)
{
  GArray *array;
  char **languages;
  guint i;
  languages = g_settings_get_strv (settings, key);

  array = g_array_new (TRUE, FALSE, sizeof (char *));

  for (i = 0; languages[i]; i++) {
    if (g_strcmp0 (languages[i], "system") == 0) {
      ephy_langs_append_languages (array);
    } else if (languages[i][0] != '\0') {
      char *str = g_strdup (languages[i]);
      g_array_append_val (array, str);
    }
  }
  g_strfreev (languages);

  if (!array->len)
    ephy_langs_append_languages (array);
  ephy_langs_sanitise (array);

  g_object_set (engine_settings, "preferred-languages", array->data, NULL);

  for (i = 0; i < array->len; i++)
    g_free (g_array_index (array, char *, i));
  g_array_free (array, TRUE);
}

static void
engine_pref_callback_gnome_fonts (GSettings  *ephy_settings,
                                  const char *key,
                                  gpointer    data)
{
  if (g_settings_get_boolean (ephy_settings, key)) {
    g_object_set (engine_settings,
                  "default-font-family", "serif",
                  "serif-font-family", "serif",
                  "sans-serif-font-family", "sans-serif",
                  "monospace-font-family", "monospace",
                  "default-font-size", 16,
                  "default-monospace-font-size", 13,
                  NULL);
  } else {
    /* Sync with Epiphany values */
    engine_pref_callback_font_size (ephy_settings, EPHY_PREFS_WEB_SERIF_FONT,
                                    (gpointer)"default-font-size");
    engine_pref_callback_font_size (ephy_settings, EPHY_PREFS_WEB_MONOSPACE_FONT,
                                    (gpointer)"default-monospace-font-size");

    engine_pref_callback_font_family (ephy_settings, EPHY_PREFS_WEB_SERIF_FONT,
                                      (gpointer)"default-font-family");
    engine_pref_callback_font_family (ephy_settings, EPHY_PREFS_WEB_SANS_SERIF_FONT,
                                      (gpointer)"sans-serif-font-family");
    engine_pref_callback_font_family (ephy_settings, EPHY_PREFS_WEB_MONOSPACE_FONT,
                                      (gpointer)"monospace-font-family");
    engine_pref_callback_font_family (ephy_settings, EPHY_PREFS_WEB_SERIF_FONT,
                                      (gpointer)"serif-font-family");
  }
}

static void
engine_pref_callback_enable_spell_checking (GSettings  *settings,
                                            const char *key,
                                            gpointer    data)
{
  g_object_set (engine_settings, "enable-spell-checking",
                g_settings_get_boolean (settings, key), NULL);
}

static void
engine_pref_callback_enable_website_data_storage (GSettings  *settings,
                                                  const char *key,
                                                  gpointer    data)
{
  g_object_set (engine_settings, "enable-website-data-storage",
                g_settings_get_boolean (settings, key), NULL);
}

static void
engine_pref_callback_hardware_acceleration_policy (GSettings  *settings,
                                                   const char *key,
                                                   gpointer    data)
{
  g_object_set (engine_settings, "hardware-acceleration",
                g_settings_get_enum (settings, key) != EPHY_PREFS_WEB_HARDWARE_ACCELERATION_POLICY_NEVER,
                NULL);
}

static const PrefData engine_pref_entries[] = {
  /* Epiphany font settings */
  {
    EPHY_PREFS_WEB_SCHEMA,
    EPHY_PREFS_WEB_SERIF_FONT,
    "default-font-size",
    engine_pref_callback_font_size
  },
  {
    EPHY_PREFS_WEB_SCHEMA,
    EPHY_PREFS_WEB_MONOSPACE_FONT,
    "default-monospace-font-size",
    engine_pref_callback_font_size
  },
  {
    EPHY_PREFS_WEB_SCHEMA,
    EPHY_PREFS_WEB_SERIF_FONT,
    "default-font-family",
    engine_pref_callback_font_family
  },
  {
    EPHY_PREFS_WEB_SCHEMA,
    EPHY_PREFS_WEB_SANS_SERIF_FONT,
    "sans-serif-font-family",
    engine_pref_callback_font_family
  },
  {
    EPHY_PREFS_WEB_SCHEMA,
    EPHY_PREFS_WEB_MONOSPACE_FONT,
    "monospace-font-family",
    engine_pref_callback_font_family
  },
  {
    EPHY_PREFS_WEB_SCHEMA,
    EPHY_PREFS_WEB_SERIF_FONT,
    "serif-font-family",
    engine_pref_callback_font_family
  },

  {
    EPHY_PREFS_WEB_SCHEMA,
    EPHY_PREFS_WEB_USE_GNOME_FONTS,
    NULL,
    engine_pref_callback_gnome_fonts
  },

  {
    EPHY_PREFS_WEB_SCHEMA,
    EPHY_PREFS_WEB_ENABLE_SPELL_CHECKING,
    NULL,
    engine_pref_callback_enable_spell_checking
  },

  {
    EPHY_PREFS_WEB_SCHEMA,
    EPHY_PREFS_WEB_ENABLE_USER_CSS,
    "user-stylesheet-uri",
    engine_pref_callback_user_stylesheet
  },
  {
    EPHY_PREFS_WEB_SCHEMA,
    EPHY_PREFS_WEB_ENABLE_USER_JS,
    "user-javascript-uri",
    engine_pref_callback_user_javascript
  },
  {
    EPHY_PREFS_WEB_SCHEMA,
    EPHY_PREFS_WEB_LANGUAGE,
    "accept-language",
    engine_pref_callback_accept_languages
  },
  {
    EPHY_PREFS_WEB_SCHEMA,
    EPHY_PREFS_WEB_USER_AGENT,
    "user-agent",
    engine_pref_callback_user_agent
  },
  {
    EPHY_PREFS_WEB_SCHEMA,
    EPHY_PREFS_WEB_ENABLE_WEBSITE_DATA_STORAGE,
    NULL,
    engine_pref_callback_enable_website_data_storage
  },
  {
    EPHY_PREFS_WEB_SCHEMA,
    EPHY_PREFS_WEB_HARDWARE_ACCELERATION_POLICY,
    "hardware-acceleration-policy",
    engine_pref_callback_hardware_acceleration_policy
  },
};

static gboolean
feature_overrides_get_mapping (GValue   *value,
                               GVariant *variant,
                               gpointer  user_data)
{
  g_value_set_variant (value, variant);
  return TRUE;
}

static GVariant *
feature_overrides_set_mapping (const GValue       *value,
                               const GVariantType *expected_type,
                               gpointer            user_data)
{
  GVariant *variant = g_value_get_variant (value);
  return variant ? g_variant_ref (variant) : g_variant_new_array (G_VARIANT_TYPE ("{sb}"), NULL, 0);
}

static gpointer
ephy_embed_prefs_init (gpointer user_data)
{
  guint i;

  engine_settings = ephy_engine_settings_get_default ();

  for (i = 0; i < G_N_ELEMENTS (engine_pref_entries); i++) {
    GSettings *settings;
    char *key;

    settings = ephy_settings_get (engine_pref_entries[i].schema);
    key = g_strconcat ("changed::", engine_pref_entries[i].key, NULL);

    engine_pref_entries[i].callback (settings,
                                     engine_pref_entries[i].key,
                                     (gpointer)engine_pref_entries[i].engine_pref);

    g_signal_connect (settings, key,
                      G_CALLBACK (engine_pref_entries[i].callback),
                      (gpointer)engine_pref_entries[i].engine_pref);
    g_free (key);
  }

  g_settings_bind_with_mapping (EPHY_SETTINGS_MAIN, "brave-adapter-overrides",
                                engine_settings, "brave-adapter-overrides", G_SETTINGS_BIND_GET,
                                feature_overrides_get_mapping, NULL, NULL, NULL);
  g_settings_bind (EPHY_SETTINGS_WEB, EPHY_PREFS_WEB_ENABLE_VIM_MODE,
                   engine_settings, "enable-vim-mode", G_SETTINGS_BIND_GET);
  g_settings_bind (EPHY_SETTINGS_WEB, EPHY_PREFS_WEB_ENABLE_NAVIGATION_GESTURES,
                   engine_settings, "enable-navigation-gestures", G_SETTINGS_BIND_GET);
  g_settings_bind (EPHY_SETTINGS_WEB, EPHY_PREFS_WEB_ENABLE_ADBLOCK,
                   engine_settings, "enable-adblock", G_SETTINGS_BIND_GET);
  g_settings_bind (EPHY_SETTINGS_WEB, EPHY_PREFS_WEB_ENABLE_POPUPS,
                   engine_settings, "allow-popups", G_SETTINGS_BIND_GET);
  g_settings_bind (EPHY_SETTINGS_WEB, EPHY_PREFS_WEB_ENABLE_COOKIE_BANNER,
                   engine_settings, "block-cookie-banners", G_SETTINGS_BIND_GET);
  g_settings_bind (EPHY_SETTINGS_MAIN,
                   EPHY_PREFS_ENABLE_CARET_BROWSING,
                   engine_settings, "enable-caret-browsing",
                   G_SETTINGS_BIND_GET);
  g_settings_bind (EPHY_SETTINGS_WEB,
                   EPHY_PREFS_WEB_FONT_MIN_SIZE,
                   engine_settings, "minimum-font-size",
                   G_SETTINGS_BIND_GET);
  g_settings_bind (EPHY_SETTINGS_WEB,
                   EPHY_PREFS_WEB_DEFAULT_ENCODING,
                   engine_settings, DEFAULT_ENCODING_SETTING,
                   G_SETTINGS_BIND_GET);
  g_settings_bind_with_mapping (EPHY_SETTINGS_WEB, "chromium-feature-overrides",
                                engine_settings, "feature-overrides", G_SETTINGS_BIND_DEFAULT,
                                feature_overrides_get_mapping, feature_overrides_set_mapping,
                                NULL, NULL);

  return engine_settings;
}

EphyEngineSettings *
ephy_embed_prefs_get_settings (void)
{
  static GOnce once_init = G_ONCE_INIT;

  return g_once (&once_init, ephy_embed_prefs_init, NULL);
}

void
ephy_embed_prefs_apply_user_style (WebKitUserContentManager *ucm)
{
  if (style_sheet)
    webkit_user_content_manager_add_style_sheet (ucm, style_sheet);
}

void
ephy_embed_prefs_apply_user_javascript (WebKitUserContentManager *ucm)
{
  if (javascript)
    webkit_user_content_manager_add_script (ucm, javascript);
}

void
ephy_embed_prefs_register_ucm (WebKitUserContentManager *ucm)
{
  ucm_list = g_list_append (ucm_list, ucm);
}

void
ephy_embed_prefs_unregister_ucm (WebKitUserContentManager *ucm)
{
  ucm_list = g_list_remove (ucm_list, ucm);
}
