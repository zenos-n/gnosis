#include "ephy-engine-scheme.h"
/* -*- Mode: C; tab-width: 2; indent-tabs-mode: nil; c-basic-offset: 2 -*- */
/*
 *  Copyright © 2000-2003 Marco Pesenti Gritti
 *  Copyright © 2011 Igalia S.L.
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

#include "ephy-embed-shell.h"

#include <stdlib.h>
#include <string.h>

#include <glib/gi18n.h>
#include <gtk/gtk.h>

#include "ephy-about-handler.h"
#include "ephy-autofill-field.h"
#include "ephy-debug.h"
#include "ephy-downloads-manager.h"
#include "ephy-embed-container.h"
#include "ephy-embed-prefs.h"
#include "ephy-embed-type-builtins.h"
#include "ephy-embed-utils.h"
#include "ephy-encodings.h"
#include "ephy-file-helpers.h"
#include "ephy-flatpak-utils.h"
#include "ephy-history-service.h"
#include "ephy-password-manager.h"
#include "ephy-profile-utils.h"
#include "ephy-reader-handler.h"
#include "ephy-settings.h"
#include "ephy-snapshot-service.h"
#include "ephy-tabs-catalog.h"
#include "ephy-uri-helpers.h"
#include "ephy-engine-origin.h"
#include "ephy-view-source-handler.h"
#include "ephy-web-app-utils.h"

#define PAGE_SETUP_FILENAME "page-setup-gtk.ini"
#define PRINT_SETTINGS_FILENAME "print-settings.ini"

typedef struct {
  EphyEngineSession *network_session;
  /* Removed when the website-data and download services finish migrating. */
  WebKitNetworkSession *legacy_network_session;
  EphyHistoryService *global_history_service;
  EphyEncodings *encodings;
  GtkPageSetup *page_setup;
  GtkPrintSettings *print_settings;
  EphyEmbedShellMode mode;
  EphyDownloadsManager *downloads_manager;
  EphyPermissionsManager *permissions_manager;
  EphyPasswordManager *password_manager;
  EphyAboutHandler *about_handler;
  EphyViewSourceHandler *source_handler;
  EphyReaderHandler *reader_handler;
  char *guid;
  EphySearchEngineManager *search_engine_manager;
  GCancellable *cancellable;
  GCancellable *overview_cancellable;
  guint64 overview_generation;
  GHashTable *overview_urls;
} EphyEmbedShellPrivate;

enum {
  RESTORED_WINDOW,
  WEB_VIEW_CREATED,
  PASSWORD_FORM_FOCUSED,
  PASSWORD_FORM_SUBMITTED,
  AUTOFILL_SIGNAL,

  LAST_SIGNAL
};

static guint signals[LAST_SIGNAL];

typedef enum {
  PROP_MODE = 1,
} EphyEmbedShellProps;

static GParamSpec *object_properties[PROP_MODE + 1] = { NULL, };

static EphyEmbedShell *embed_shell = NULL;

static void ephy_embed_shell_tabs_catalog_iface_init (EphyTabsCatalogInterface *iface);

G_DEFINE_TYPE_WITH_CODE (EphyEmbedShell, ephy_embed_shell, ADW_TYPE_APPLICATION,
                         G_ADD_PRIVATE (EphyEmbedShell)
                         G_IMPLEMENT_INTERFACE (EPHY_TYPE_TABS_CATALOG,
                                                ephy_embed_shell_tabs_catalog_iface_init))

static EphyWebView *
ephy_embed_shell_get_view_for_page_id (EphyEmbedShell *self,
                                       guint64         page_id,
                                       const char     *origin)
{
  GList *windows = gtk_application_get_windows (GTK_APPLICATION (self));

  for (GList *l = windows; l && l->data; l = l->next) {
    g_autoptr (GList) tabs = NULL;

    if (!EPHY_IS_EMBED_CONTAINER (l->data))
      continue;
    tabs = ephy_embed_container_get_children (l->data);

    for (GList *t = tabs; t && t->data; t = t->next) {
      EphyWebView *ephy_view = ephy_embed_get_web_view (t->data);
      EphyEngineView *web_view = EPHY_ENGINE_VIEW (ephy_view);
      g_autofree char *real_origin = NULL;

      if (ephy_engine_web_view_get_page_id (web_view) != page_id)
        continue;

      real_origin = ephy_uri_to_security_origin (ephy_engine_web_view_get_uri (web_view));

      if (g_strcmp0 (real_origin, origin) != 0) {
        g_debug ("Extension's origin '%s' doesn't match real origin '%s'", origin, real_origin);
        return NULL;
      }

      return ephy_view;
    }
  }

  return NULL;
}

static GList *
tabs_catalog_get_tabs_info (EphyTabsCatalog *catalog)
{
  EphyEngineFaviconDatabase *database;
  GList *windows;
  g_autoptr (GList) tabs = NULL;
  GList *tabs_info = NULL;
  const char *title;
  const char *url;
  g_autofree char *favicon = NULL;

  g_assert ((gpointer)catalog == (gpointer)embed_shell);

  windows = gtk_application_get_windows (GTK_APPLICATION (embed_shell));
  database = ephy_embed_shell_get_favicon_database (embed_shell);

  for (GList *l = windows; l && l->data; l = l->next) {
    if (!EPHY_IS_EMBED_CONTAINER (l->data))
      continue;
    g_clear_pointer (&tabs, g_list_free);
    tabs = ephy_embed_container_get_children (l->data);

    for (GList *t = tabs; t && t->data; t = t->next) {
      title = ephy_embed_get_title (t->data);

      if (g_strcmp0 (title, _(BLANK_PAGE_TITLE)) == 0 ||
          g_strcmp0 (title, _(NEW_TAB_PAGE_TITLE)) == 0)
        continue;

      url = ephy_web_view_get_display_address (ephy_embed_get_web_view (t->data));
      g_free (favicon);
      favicon = ephy_engine_favicon_database_get_favicon_uri (database, url);

      tabs_info = g_list_prepend (tabs_info,
                                  ephy_tab_info_new (title, url, favicon));
    }
  }

  return tabs_info;
}

static void
ephy_embed_shell_tabs_catalog_iface_init (EphyTabsCatalogInterface *iface)
{
  iface->get_tabs_info = tabs_catalog_get_tabs_info;
}

static void
ephy_embed_shell_dispose (GObject *object)
{
  EphyEmbedShellPrivate *priv = ephy_embed_shell_get_instance_private (EPHY_EMBED_SHELL (object));

  if (priv->cancellable) {
    g_cancellable_cancel (priv->cancellable);
    g_clear_object (&priv->cancellable);
  }

  if (priv->overview_cancellable)
    g_cancellable_cancel (priv->overview_cancellable);
  g_clear_object (&priv->overview_cancellable);
  g_clear_pointer (&priv->overview_urls, g_hash_table_unref);

  g_clear_object (&priv->encodings);
  g_clear_object (&priv->page_setup);
  g_clear_object (&priv->print_settings);
  g_clear_object (&priv->global_history_service);
  g_clear_object (&priv->about_handler);
  g_clear_object (&priv->reader_handler);
  g_clear_object (&priv->source_handler);
  g_clear_object (&priv->downloads_manager);
  g_clear_object (&priv->password_manager);
  g_clear_object (&priv->permissions_manager);
  g_clear_object (&priv->network_session);
  g_clear_object (&priv->legacy_network_session);
  g_clear_pointer (&priv->guid, g_free);
  g_clear_object (&priv->search_engine_manager);

  G_OBJECT_CLASS (ephy_embed_shell_parent_class)->dispose (object);
}

static void
web_process_extension_password_form_focused_message_received_cb (WebKitUserContentManager *manager,
                                                                 JSCValue                 *message,
                                                                 EphyEmbedShell           *shell)
{
  guint64 page_id;
  gboolean insecure_form_action;
  g_autoptr (GVariant) variant = NULL;
  g_autofree char *message_str = NULL;

  message_str = jsc_value_to_string (message);
  variant = g_variant_parse (G_VARIANT_TYPE ("(tb)"), message_str, NULL, NULL, NULL);

  g_variant_get (variant, "(tb)", &page_id, &insecure_form_action);
  g_signal_emit (shell, signals[PASSWORD_FORM_FOCUSED], 0,
                 page_id, insecure_form_action);
}

static GtkWindow *
overview_get_window (EphyEmbedShell *shell,
                     EphyEngineView *view)
{
  GtkRoot *root = gtk_widget_get_root (GTK_WIDGET (view));
  g_autoptr (GList) children = NULL;

  if (!GTK_IS_APPLICATION_WINDOW (root) ||
      gtk_window_get_application (GTK_WINDOW (root)) != GTK_APPLICATION (shell) ||
      !EPHY_IS_EMBED_CONTAINER (root))
    return NULL;

  children = ephy_embed_container_get_children (EPHY_EMBED_CONTAINER (root));
  for (GList *l = children; l; l = l->next) {
    if (EPHY_ENGINE_VIEW (ephy_embed_get_web_view (l->data)) == view)
      return GTK_WINDOW (root);
  }

  return NULL;
}

gboolean
ephy_embed_shell_can_show_overview (EphyEmbedShell *shell,
                                   EphyEngineView *view)
{
  EphyEmbedShellPrivate *priv = ephy_embed_shell_get_instance_private (shell);
  g_autoptr (EphyEngineSession) session = NULL;

  if (!EPHY_IS_ENGINE_VIEW (view) ||
      priv->mode == EPHY_EMBED_SHELL_MODE_PRIVATE ||
      priv->mode == EPHY_EMBED_SHELL_MODE_INCOGNITO ||
      priv->mode == EPHY_EMBED_SHELL_MODE_AUTOMATION ||
      priv->mode == EPHY_EMBED_SHELL_MODE_SEARCH_PROVIDER ||
      !priv->network_session || ephy_engine_session_is_ephemeral (priv->network_session) ||
      !ephy_engine_web_view_has_native_uri (view, "ephy-about:overview") ||
      !overview_get_window (shell, view))
    return FALSE;

  g_object_get (view, "network-session", &session, NULL);
  return session && session == priv->network_session && !ephy_engine_session_is_ephemeral (session);
}

static void
overview_broadcast (EphyEmbedShell *shell,
                    JsonNode       *message)
{
  GList *windows = gtk_application_get_windows (GTK_APPLICATION (shell));

  for (GList *l = windows; l; l = l->next) {
    g_autoptr (GList) children = NULL;

    if (!EPHY_IS_EMBED_CONTAINER (l->data))
      continue;
    children = ephy_embed_container_get_children (EPHY_EMBED_CONTAINER (l->data));
    for (GList *t = children; t; t = t->next) {
      EphyEngineView *view = EPHY_ENGINE_VIEW (ephy_embed_get_web_view (t->data));

      if (ephy_embed_shell_can_show_overview (shell, view) &&
          ephy_engine_web_view_is_current_page (view, "ephy-about:overview"))
        ephy_engine_web_view_send_page_message (view, "ephy-about:overview", "overview", message);
    }
  }
}

char *
ephy_embed_shell_get_overview_thumbnail_uri (EphyEmbedShell *shell,
                                            const char     *url)
{
  EphyEmbedShellPrivate *priv = ephy_embed_shell_get_instance_private (shell);
  g_autofree char *escaped = NULL;
  g_autoptr (GFile) file = NULL;
  g_autoptr (GFileInfo) info = NULL;
  const char *path;

  if (!url || !priv->overview_urls || !g_hash_table_contains (priv->overview_urls, url))
    return NULL;
  path = ephy_snapshot_service_lookup_cached_snapshot_path (ephy_snapshot_service_get_default (), url);
  if (!path)
    return NULL;
  file = g_file_new_for_path (path);
  info = g_file_query_info (file,
                            G_FILE_ATTRIBUTE_TIME_MODIFIED ","
                            G_FILE_ATTRIBUTE_TIME_MODIFIED_USEC ","
                            G_FILE_ATTRIBUTE_STANDARD_SIZE,
                            G_FILE_QUERY_INFO_NONE, NULL, NULL);
  if (!info)
    return NULL;

  escaped = g_uri_escape_string (url, NULL, FALSE);
  /* Chromium embeds images through the resource scheme. The endpoint below
   * still authorizes the owning overview and resolves only native cache URLs. */
  return g_strdup_printf ("ephy-resource:///org/gnome/epiphany/overview-thumbnail?url=%s&v=%" G_GUINT64_FORMAT "-%u-%" G_GOFFSET_FORMAT,
                           escaped,
                           g_file_info_get_attribute_uint64 (info, G_FILE_ATTRIBUTE_TIME_MODIFIED),
                           g_file_info_get_attribute_uint32 (info, G_FILE_ATTRIBUTE_TIME_MODIFIED_USEC),
                           g_file_info_get_size (info));
}

typedef struct {
  GWeakRef shell;
  guint64 generation;
} OverviewQuery;

static void
history_service_query_urls_cb (EphyHistoryService *service,
                               GAsyncResult       *result,
                               OverviewQuery      *query)
{
  g_autoptr (EphyEmbedShell) shell = g_weak_ref_get (&query->shell);
  g_autolist (EphyHistoryURL) urls = NULL;
  g_autoptr (GError) error = NULL;
  g_autoptr (JsonBuilder) builder = NULL;
  g_autoptr (JsonNode) message = NULL;
  EphyEmbedShellPrivate *priv;
  guint64 generation = query->generation;

  g_weak_ref_clear (&query->shell);
  g_free (query);
  urls = ephy_history_service_query_urls_finish (service, result, &error);
  if (!shell)
    return;
  priv = ephy_embed_shell_get_instance_private (shell);
  if (generation != priv->overview_generation || !priv->overview_cancellable ||
      g_cancellable_is_cancelled (priv->overview_cancellable))
    return;
  if (error) {
    if (!g_error_matches (error, G_IO_ERROR, G_IO_ERROR_CANCELLED))
      g_warning ("Failed to query overview URLs: %s", error->message);
    return;
  }

  g_hash_table_remove_all (priv->overview_urls);
  for (GList *l = urls; l; l = l->next) {
    EphyHistoryURL *url = l->data;

    g_hash_table_add (priv->overview_urls, g_strdup (url->url));
  }

  builder = json_builder_new ();
  json_builder_begin_object (builder);
  json_builder_set_member_name (builder, "type");
  json_builder_add_string_value (builder, "urls");
  json_builder_set_member_name (builder, "urls");
  json_builder_begin_array (builder);
  for (GList *l = urls; l; l = l->next) {
    EphyHistoryURL *url = l->data;
    g_autofree char *thumbnail = ephy_embed_shell_get_overview_thumbnail_uri (shell, url->url);

    json_builder_begin_object (builder);
    json_builder_set_member_name (builder, "url");
    json_builder_add_string_value (builder, url->url);
    json_builder_set_member_name (builder, "title");
    json_builder_add_string_value (builder, url->title ? url->title : "");
    json_builder_set_member_name (builder, "pinned");
    json_builder_add_boolean_value (builder, url->pinned);
    json_builder_set_member_name (builder, "thumbnail");
    if (thumbnail)
      json_builder_add_string_value (builder, thumbnail);
    else
      json_builder_add_null_value (builder);
    json_builder_end_object (builder);
  }
  json_builder_end_array (builder);
  json_builder_end_object (builder);
  message = json_builder_get_root (builder);
  overview_broadcast (shell, message);

  for (GList *l = urls; l; l = l->next)
    ephy_embed_shell_schedule_thumbnail_update (shell, l->data);
}

void
ephy_embed_shell_update_overview_urls (EphyEmbedShell *shell)
{
  EphyEmbedShellPrivate *priv = ephy_embed_shell_get_instance_private (shell);
  g_autoptr (EphyHistoryQuery) query = NULL;
  OverviewQuery *request;

  if (!priv->network_session || ephy_engine_session_is_ephemeral (priv->network_session) ||
      priv->mode == EPHY_EMBED_SHELL_MODE_PRIVATE ||
      priv->mode == EPHY_EMBED_SHELL_MODE_INCOGNITO ||
      priv->mode == EPHY_EMBED_SHELL_MODE_AUTOMATION ||
      priv->mode == EPHY_EMBED_SHELL_MODE_SEARCH_PROVIDER)
    return;

  if (priv->overview_cancellable)
    g_cancellable_cancel (priv->overview_cancellable);
  g_clear_object (&priv->overview_cancellable);
  priv->overview_cancellable = g_cancellable_new ();
  if (!priv->overview_urls)
    priv->overview_urls = g_hash_table_new_full (g_str_hash, g_str_equal, g_free, NULL);

  request = g_new0 (OverviewQuery, 1);
  g_weak_ref_init (&request->shell, shell);
  request->generation = ++priv->overview_generation;
  query = ephy_history_query_new_for_overview ();
  ephy_history_service_query_urls (ephy_embed_shell_get_global_history_service (shell), query,
                                   priv->overview_cancellable,
                                   (GAsyncReadyCallback)history_service_query_urls_cb, request);
}

static void
history_service_urls_visited_cb (EphyHistoryService *history,
                                 EphyEmbedShell     *shell)
{
  ephy_embed_shell_update_overview_urls (shell);
}

typedef struct {
  GWeakRef shell;
  GWeakRef view;
  char *url;
  gboolean pin;
  gboolean show_toast;
  gulong load_changed_handler;
} OverviewMutation;

static void
overview_mutation_load_changed_cb (EphyEngineView  *view,
                                   WebKitLoadEvent  event,
                                   OverviewMutation *mutation)
{
  if (event == WEBKIT_LOAD_STARTED)
    mutation->show_toast = FALSE;
}

static void
history_overview_mutation_cb (EphyHistoryService *service,
                              GAsyncResult       *result,
                              OverviewMutation   *mutation)
{
  g_autoptr (EphyEmbedShell) shell = g_weak_ref_get (&mutation->shell);
  g_autoptr (EphyEngineView) view = g_weak_ref_get (&mutation->view);
  g_autoptr (GError) error = NULL;
  gboolean success;

  if (view)
    g_signal_handler_disconnect (view, mutation->load_changed_handler);

  success = mutation->pin ? ephy_history_service_set_url_pinned_finish (service, result, &error)
                         : ephy_history_service_set_url_hidden_finish (service, result, &error);
  if (!success) {
    if (!g_error_matches (error, G_IO_ERROR, G_IO_ERROR_CANCELLED))
      g_warning ("Failed to update overview URL: %s", error ? error->message : "history entry no longer exists");
  } else if (shell) {
    ephy_embed_shell_update_overview_urls (shell);
    if (!mutation->pin && mutation->show_toast && view &&
        ephy_embed_shell_can_show_overview (shell, view) &&
        ephy_engine_web_view_is_current_page (view, "ephy-about:overview"))
      gtk_widget_activate_action (GTK_WIDGET (overview_get_window (shell, view)),
                                  "win.show-overview-undo-toast", "s", mutation->url);
  }

  g_weak_ref_clear (&mutation->shell);
  g_weak_ref_clear (&mutation->view);
  g_free (mutation->url);
  g_free (mutation);
}

void
ephy_embed_shell_handle_overview_message (EphyEmbedShell *shell,
                                          EphyEngineView *view,
                                          JsonNode       *message)
{
  EphyEmbedShellPrivate *priv = ephy_embed_shell_get_instance_private (shell);
  JsonObject *object;
  JsonNode *action_node, *url_node, *pinned_node;
  const char *action, *url;
  OverviewMutation *mutation;
  gboolean pin;

  if (!ephy_embed_shell_can_show_overview (shell, view) ||
      !ephy_engine_web_view_is_current_page (view, "ephy-about:overview") || !message ||
      !JSON_NODE_HOLDS_OBJECT (message))
    return;
  object = json_node_get_object (message);
  action_node = json_object_get_member (object, "action");
  if (!action_node || json_node_get_value_type (action_node) != G_TYPE_STRING)
    return;
  action = json_node_get_string (action_node);
  if (g_str_equal (action, "ready")) {
    if (json_object_get_size (object) == 1)
      ephy_embed_shell_update_overview_urls (shell);
    return;
  }
  pin = g_str_equal (action, "pin");
  if (!pin && !g_str_equal (action, "remove"))
    return;
  url_node = json_object_get_member (object, "url");
  if (!url_node || json_node_get_value_type (url_node) != G_TYPE_STRING)
    return;
  url = json_node_get_string (url_node);
  if (!url || !*url || !priv->overview_urls || !g_hash_table_contains (priv->overview_urls, url))
    return;
  pinned_node = json_object_get_member (object, "pinned");
  if (json_object_get_size (object) != (pin ? 3 : 2) ||
      (pin && (!pinned_node || json_node_get_value_type (pinned_node) != G_TYPE_BOOLEAN)))
    return;

  mutation = g_new0 (OverviewMutation, 1);
  g_weak_ref_init (&mutation->shell, shell);
  g_weak_ref_init (&mutation->view, view);
  mutation->url = g_strdup (url);
  mutation->pin = pin;
  mutation->show_toast = TRUE;
  mutation->load_changed_handler = g_signal_connect (view, "load-changed",
                                                     G_CALLBACK (overview_mutation_load_changed_cb), mutation);
  if (pin)
    ephy_history_service_set_url_pinned (priv->global_history_service, url,
                                         json_node_get_boolean (pinned_node), priv->cancellable,
                                         (GAsyncReadyCallback)history_overview_mutation_cb, mutation);
  else
    ephy_history_service_set_url_hidden (priv->global_history_service, url, TRUE, priv->cancellable,
                                         (GAsyncReadyCallback)history_overview_mutation_cb, mutation);
}

static char *
property_to_string_or_null (JSCValue   *value,
                            const char *name)
{
  g_autoptr (JSCValue) prop = jsc_value_object_get_property (value, name);
  if (jsc_value_is_null (prop) || jsc_value_is_undefined (prop))
    return NULL;
  return jsc_value_to_string (prop);
}

static guint64
property_to_uint64 (JSCValue   *value,
                    const char *name)
{
  g_autoptr (JSCValue) prop = jsc_value_object_get_property (value, name);
  return (guint64)jsc_value_to_double (prop);
}

static gboolean
property_to_boolean (JSCValue   *value,
                     const char *name)
{
  g_autoptr (JSCValue) prop = jsc_value_object_get_property (value, name);
  return jsc_value_to_boolean (prop);
}

static void
web_process_extension_autofill_askuser_received_cb (WebKitUserContentManager *manager,
                                                    JSCValue                 *value,
                                                    EphyEmbedShell           *shell)
{
  guint64 page_id = property_to_uint64 (value, "pageId");
  guint64 frame_id = property_to_uint64 (value, "frameId");
  char *selector = property_to_string_or_null (value, "selector");
  gboolean is_fillable_element = property_to_boolean (value, "isFillableElement");
  gboolean has_personal_fields = property_to_boolean (value, "hasPersonalFields");
  gboolean has_card_fields = property_to_boolean (value, "hasCardFields");
  guint64 x = property_to_uint64 (value, "x");
  guint64 y = property_to_uint64 (value, "y");
  guint64 element_width = property_to_uint64 (value, "width");
  guint64 element_height = property_to_uint64 (value, "height");

  g_signal_emit (shell, signals[AUTOFILL_SIGNAL], 0,
                 page_id, frame_id, selector, is_fillable_element, has_personal_fields, has_card_fields, x, y, element_width, element_height);
}

static void
web_process_extension_password_manager_save_real (EphyEmbedShell *shell,
                                                  JSCValue       *value,
                                                  gboolean        is_request)
{
  EphyEmbedShellPrivate *priv = ephy_embed_shell_get_instance_private (shell);
  g_autofree char *origin = property_to_string_or_null (value, "origin");
  g_autofree char *target_origin = property_to_string_or_null (value, "targetOrigin");
  g_autofree char *username = property_to_string_or_null (value, "username");
  g_autofree char *password = property_to_string_or_null (value, "password");
  g_autofree char *username_field = property_to_string_or_null (value, "usernameField");
  g_autofree char *password_field = property_to_string_or_null (value, "passwordField");
  g_autoptr (JSCValue) is_new_prop = jsc_value_object_get_property (value, "isNew");
  gboolean is_new = jsc_value_to_boolean (is_new_prop);
  guint64 page_id = property_to_uint64 (value, "pageID");
  EphyWebView *view;
  EphyPasswordRequestData *request_data;

  /* Both origin and target origin are required. */
  if (!origin || !target_origin)
    return;

  /* Both password and password field are required. */
  if (!password || !password_field)
    return;

  /* The username field is required if username is present. */
  if (username && !username_field)
    g_clear_pointer (&username, g_free);

  /* The username is required if username field is present. */
  if (!username && username_field)
    g_clear_pointer (&username_field, g_free);

  /* This also sanity checks that a page isn't saving websites for
   * other origins. Remember the request comes from the untrusted web
   * process and we have to make sure it's not being evil here. This
   * could also happen even without malice if the origin of a subframe
   * doesn't match the origin of the main frame (in which case we'll
   * refuse to save the password).
   */
  view = ephy_embed_shell_get_view_for_page_id (shell, page_id, origin);
  if (!view)
    return;

  /* Since we're automatically storing the password, no user interaction is required,
  so we don't need to send the data anywhere */
  if (!is_request) {
    ephy_password_manager_save (priv->password_manager, origin, target_origin, username,
                                username, password, username_field, password_field, is_new);
    return;
  } else {
    /* User interaction is required, so whatever is handling the EmbedShell should
    handle the request + saving the password */

    request_data = g_new (EphyPasswordRequestData, 1);
    request_data->origin = g_steal_pointer (&origin);
    request_data->target_origin = g_steal_pointer (&target_origin);
    request_data->username = g_steal_pointer (&username);
    request_data->password = g_steal_pointer (&password);
    request_data->usernameField = g_steal_pointer (&username_field);
    request_data->passwordField = g_steal_pointer (&password_field);
    request_data->isNew = is_new;
    g_signal_emit (shell, signals[PASSWORD_FORM_SUBMITTED], 0, request_data);
  }
}

static void
web_process_extension_password_manager_save_received_cb (WebKitUserContentManager *manager,
                                                         JSCValue                 *message,
                                                         EphyEmbedShell           *shell)
{
  web_process_extension_password_manager_save_real (shell, message, FALSE);
}

static void
web_process_extension_password_manager_request_save_received_cb (WebKitUserContentManager *manager,
                                                                 JSCValue                 *message,
                                                                 EphyEmbedShell           *shell)
{
  web_process_extension_password_manager_save_real (shell, message, TRUE);
}

static void
history_service_url_title_changed_cb (EphyHistoryService *service,
                                      const char         *url,
                                      const char         *title,
                                      EphyEmbedShell     *shell)
{
  g_autoptr (JsonObject) object = json_object_new ();
  g_autoptr (JsonNode) message = json_node_new (JSON_NODE_OBJECT);

  json_object_set_string_member (object, "type", "title");
  json_object_set_string_member (object, "url", url);
  json_object_set_string_member (object, "title", title ? title : "");
  json_node_set_object (message, object);
  overview_broadcast (shell, message);
  ephy_embed_shell_update_overview_urls (shell);
}

static void
history_service_url_deleted_cb (EphyHistoryService *service,
                                EphyHistoryURL     *url,
                                EphyEmbedShell     *shell)
{
  EphyEmbedShellPrivate *priv = ephy_embed_shell_get_instance_private (shell);

  if (priv->overview_urls)
    g_hash_table_remove (priv->overview_urls, url->url);
  ephy_embed_shell_update_overview_urls (shell);
}

static void
history_service_host_deleted_cb (EphyHistoryService *service,
                                 const char         *deleted_url,
                                 EphyEmbedShell     *shell)
{
  EphyEmbedShellPrivate *priv = ephy_embed_shell_get_instance_private (shell);

  if (priv->overview_urls)
    g_hash_table_remove_all (priv->overview_urls);
  ephy_embed_shell_update_overview_urls (shell);
}

static void
history_service_cleared_cb (EphyHistoryService *service,
                            EphyEmbedShell     *shell)
{
  EphyEmbedShellPrivate *priv = ephy_embed_shell_get_instance_private (shell);

  if (priv->overview_urls)
    g_hash_table_remove_all (priv->overview_urls);
  ephy_embed_shell_update_overview_urls (shell);
}

void
ephy_embed_shell_set_thumbnail_path (EphyEmbedShell *shell,
                                     const char     *url,
                                     const char     *path)
{
  g_autoptr (JsonObject) object = json_object_new ();
  g_autoptr (JsonNode) message = json_node_new (JSON_NODE_OBJECT);
  g_autofree char *thumbnail = NULL;

  /* Paths remain native: only the snapshot service's URL mapping is exposed. */
  if (g_strcmp0 (path, ephy_snapshot_service_lookup_cached_snapshot_path (
                     ephy_snapshot_service_get_default (), url)) != 0)
    return;
  thumbnail = ephy_embed_shell_get_overview_thumbnail_uri (shell, url);
  json_object_set_string_member (object, "type", "thumbnail");
  json_object_set_string_member (object, "url", url);
  if (thumbnail)
    json_object_set_string_member (object, "thumbnail", thumbnail);
  else
    json_object_set_null_member (object, "thumbnail");
  json_node_set_object (message, object);
  overview_broadcast (shell, message);
}

typedef struct {
  GWeakRef shell;
  char *url;
} OverviewSnapshot;

static void
got_snapshot_path_for_url_cb (EphySnapshotService *service,
                              GAsyncResult        *result,
                              OverviewSnapshot    *data)
{
  g_autoptr (EphyEmbedShell) shell = g_weak_ref_get (&data->shell);
  g_autofree char *snapshot = NULL;
  g_autoptr (GError) error = NULL;

  snapshot = ephy_snapshot_service_get_snapshot_path_for_url_finish (service, result, &error);
  if (snapshot && shell)
    ephy_embed_shell_set_thumbnail_path (shell, data->url, snapshot);
  else if (error && !g_error_matches (error, G_IO_ERROR, G_IO_ERROR_CANCELLED))
    g_info ("Failed to get snapshot for URL %s: %s", data->url, error->message);

  g_weak_ref_clear (&data->shell);
  g_free (data->url);
  g_free (data);
}

void
ephy_embed_shell_schedule_thumbnail_update (EphyEmbedShell *shell,
                                            EphyHistoryURL *url)
{
  EphyEmbedShellPrivate *priv = ephy_embed_shell_get_instance_private (shell);
  EphySnapshotService *service = ephy_snapshot_service_get_default ();
  const char *snapshot;

  if (!priv->network_session || ephy_engine_session_is_ephemeral (priv->network_session) ||
      priv->mode == EPHY_EMBED_SHELL_MODE_PRIVATE ||
      priv->mode == EPHY_EMBED_SHELL_MODE_INCOGNITO ||
      priv->mode == EPHY_EMBED_SHELL_MODE_AUTOMATION)
    return;

  snapshot = ephy_snapshot_service_lookup_cached_snapshot_path (service, url->url);
  if (snapshot) {
    ephy_embed_shell_set_thumbnail_path (shell, url->url, snapshot);
  } else {
    OverviewSnapshot *data = g_new0 (OverviewSnapshot, 1);

    g_weak_ref_init (&data->shell, shell);
    data->url = g_strdup (url->url);
    ephy_snapshot_service_get_snapshot_path_for_url_async (service, url->url,
                                                           priv->cancellable,
                                                           (GAsyncReadyCallback)got_snapshot_path_for_url_cb, data);
  }
}

typedef struct {
  GWeakRef shell;
  EphyEngineSchemeRequest *request;
  char *url;
} OverviewThumbnailRequest;

static void
overview_thumbnail_open_cb (GFile                    *file,
                            GAsyncResult             *result,
                            OverviewThumbnailRequest *data)
{
  g_autoptr (EphyEmbedShell) shell = g_weak_ref_get (&data->shell);
  g_autoptr (GFileInputStream) stream = NULL;
  g_autoptr (GError) error = NULL;
  g_autofree char *thumbnail = NULL;
  EphyEngineView *view = ephy_engine_uri_scheme_request_get_web_view (data->request);

  stream = g_file_read_finish (file, result, &error);
  if (shell)
    thumbnail = ephy_embed_shell_get_overview_thumbnail_uri (shell, data->url);
  if (!shell || !thumbnail || !ephy_embed_shell_can_show_overview (shell, view) ||
      !ephy_engine_web_view_is_current_page (view, "ephy-about:overview")) {
    g_clear_error (&error);
    error = g_error_new_literal (G_IO_ERROR, G_IO_ERROR_PERMISSION_DENIED, "Overview thumbnail request is no longer authorized");
  }
  if (error) {
    g_debug ("Overview thumbnail read failed: %s", error->message);
    ephy_engine_uri_scheme_request_finish_error (data->request, error);
  } else {
    g_debug ("Overview thumbnail read completed");
    ephy_engine_uri_scheme_request_finish (data->request, G_INPUT_STREAM (stream), -1, "image/jpeg");
  }

  g_weak_ref_clear (&data->shell);
  g_object_unref (data->request);
  g_free (data->url);
  g_free (data);
}

static gboolean
overview_thumbnail_revision_is_valid (const char *revision)
{
  g_auto (GStrv) parts = NULL;
  guint64 value;

  if (strlen (revision) > 48)
    return FALSE;
  parts = g_strsplit (revision, "-", -1);
  return g_strv_length (parts) == 3 &&
         g_ascii_string_to_unsigned (parts[0], 10, 0, G_MAXUINT64, &value, NULL) &&
         g_ascii_string_to_unsigned (parts[1], 10, 0, 999999, &value, NULL) &&
         g_ascii_string_to_unsigned (parts[2], 10, 0, G_MAXINT64, &value, NULL);
}

void
ephy_embed_shell_handle_overview_thumbnail_request (EphyEmbedShell          *shell,
                                                    EphyEngineSchemeRequest *request)
{
  EphyEmbedShellPrivate *priv = ephy_embed_shell_get_instance_private (shell);
  EphyEngineView *view = ephy_engine_uri_scheme_request_get_web_view (request);
  g_autoptr (GUri) uri = NULL;
  g_autoptr (GHashTable) params = NULL;
  g_autoptr (GError) error = NULL;
  g_autoptr (GFile) file = NULL;
  g_autofree char *thumbnail = NULL;
  const char *url = NULL;
  const char *revision;
  const char *path;
  OverviewThumbnailRequest *data;

  g_debug ("Overview thumbnail request received (main resource: %d)",
           ephy_engine_uri_scheme_request_is_main_resource (request));
  if (!ephy_embed_shell_can_show_overview (shell, view) ||
      !ephy_engine_web_view_is_current_page (view, "ephy-about:overview")) {
    error = g_error_new_literal (G_IO_ERROR, G_IO_ERROR_PERMISSION_DENIED, "Overview thumbnail request is not authorized");
    goto fail;
  }
  uri = g_uri_parse (ephy_engine_uri_scheme_request_get_uri (request), G_URI_FLAGS_ENCODED, &error);
  if (!uri)
    goto fail;
  if (g_strcmp0 (g_uri_get_scheme (uri), "ephy-resource") != 0 ||
      g_strcmp0 (g_uri_get_path (uri), "/org/gnome/epiphany/overview-thumbnail") != 0 ||
      (g_uri_get_host (uri) && *g_uri_get_host (uri)) ||
      g_uri_get_userinfo (uri) || g_uri_get_port (uri) != -1 ||
      g_uri_get_fragment (uri) || !g_uri_get_query (uri)) {
    error = g_error_new_literal (G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT, "Invalid overview thumbnail URI");
    goto fail;
  }
  params = g_uri_parse_params (g_uri_get_query (uri), -1, "&", G_URI_PARAMS_NONE, &error);
  if (!params)
    goto fail;
  revision = g_hash_table_lookup (params, "v");
  if (g_hash_table_size (params) != (revision ? 2 : 1) ||
      (revision && !overview_thumbnail_revision_is_valid (revision))) {
    error = g_error_new_literal (G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT, "Invalid overview thumbnail parameters");
    goto fail;
  }
  url = g_hash_table_lookup (params, "url");
  thumbnail = ephy_embed_shell_get_overview_thumbnail_uri (shell, url);
  if (!thumbnail) {
    error = g_error_new_literal (G_IO_ERROR, G_IO_ERROR_NOT_FOUND, "Overview thumbnail is not in the history snapshot cache");
    goto fail;
  }
  path = ephy_snapshot_service_lookup_cached_snapshot_path (ephy_snapshot_service_get_default (), url);
  file = g_file_new_for_path (path);
  data = g_new0 (OverviewThumbnailRequest, 1);
  g_weak_ref_init (&data->shell, shell);
  data->request = g_object_ref (request);
  data->url = g_strdup (url);
  g_file_read_async (file, G_PRIORITY_DEFAULT, priv->cancellable,
                     (GAsyncReadyCallback)overview_thumbnail_open_cb, data);
  return;

fail:
  g_debug ("Overview thumbnail request rejected: %s", error->message);
  ephy_engine_uri_scheme_request_finish_error (request, error);
}

/**
 * ephy_embed_shell_get_global_history_service:
 * @shell: the #EphyEmbedShell
 *
 * Return value: (transfer none): the global #EphyHistoryService
 **/
EphyHistoryService *
ephy_embed_shell_get_global_history_service (EphyEmbedShell *shell)
{
  EphyEmbedShellPrivate *priv = ephy_embed_shell_get_instance_private (shell);

  g_assert (EPHY_IS_EMBED_SHELL (shell));

  if (!priv->global_history_service) {
    g_autofree char *filename = NULL;
    EphySQLiteConnectionMode mode;

    if (priv->mode == EPHY_EMBED_SHELL_MODE_INCOGNITO ||
        priv->mode == EPHY_EMBED_SHELL_MODE_AUTOMATION ||
        priv->mode == EPHY_EMBED_SHELL_MODE_SEARCH_PROVIDER)
      mode = EPHY_SQLITE_CONNECTION_MODE_MEMORY;
    else
      mode = EPHY_SQLITE_CONNECTION_MODE_READWRITE;

    filename = g_build_filename (ephy_profile_dir (), EPHY_HISTORY_FILE, NULL);
    priv->global_history_service = ephy_history_service_new (filename, mode);

    g_signal_connect_object (priv->global_history_service, "urls-visited",
                             G_CALLBACK (history_service_urls_visited_cb),
                             shell, G_CONNECT_DEFAULT);
    g_signal_connect_object (priv->global_history_service, "url-title-changed",
                             G_CALLBACK (history_service_url_title_changed_cb),
                             shell, G_CONNECT_DEFAULT);
    g_signal_connect_object (priv->global_history_service, "url-deleted",
                             G_CALLBACK (history_service_url_deleted_cb),
                             shell, G_CONNECT_DEFAULT);
    g_signal_connect_object (priv->global_history_service, "host-deleted",
                             G_CALLBACK (history_service_host_deleted_cb),
                             shell, G_CONNECT_DEFAULT);
    g_signal_connect_object (priv->global_history_service, "cleared",
                             G_CALLBACK (history_service_cleared_cb),
                             shell, G_CONNECT_DEFAULT);
  }

  return priv->global_history_service;
}

/**
 * ephy_embed_shell_get_encodings:
 * @shell: the #EphyEmbedShell
 *
 * Return value: (transfer none):
 **/
EphyEncodings *
ephy_embed_shell_get_encodings (EphyEmbedShell *shell)
{
  EphyEmbedShellPrivate *priv = ephy_embed_shell_get_instance_private (shell);

  g_assert (EPHY_IS_EMBED_SHELL (shell));

  if (!priv->encodings)
    priv->encodings = ephy_encodings_new ();

  return priv->encodings;
}

void
ephy_embed_shell_restored_window (EphyEmbedShell *shell)
{
  g_signal_emit (shell, signals[RESTORED_WINDOW], 0);
}

static void
about_request_cb (EphyEngineSchemeRequest *request,
                  EphyEmbedShell         *shell)
{
  EphyEmbedShellPrivate *priv = ephy_embed_shell_get_instance_private (shell);

  ephy_about_handler_handle_request (priv->about_handler, request);
}

static void
source_request_cb (EphyEngineSchemeRequest *request,
                   EphyEmbedShell         *shell)
{
  EphyEmbedShellPrivate *priv = ephy_embed_shell_get_instance_private (shell);

  ephy_view_source_handler_handle_request (priv->source_handler, request);
}

static void
reader_request_cb (EphyEngineSchemeRequest *request,
                   EphyEmbedShell         *shell)
{
  EphyEmbedShellPrivate *priv = ephy_embed_shell_get_instance_private (shell);

  ephy_reader_handler_handle_request (priv->reader_handler, request);
}

static void
ephy_resource_request_cb (EphyEngineSchemeRequest *request)
{
  const char *path;
  gsize size;
  EphyEngineView *request_view;
  const char *uri;
  g_autoptr (GInputStream) stream = NULL;
  g_autoptr (GError) error = NULL;

  path = ephy_engine_uri_scheme_request_get_path (request);
  if (g_strcmp0 (path, "/org/gnome/epiphany/overview-thumbnail") == 0) {
    ephy_embed_shell_handle_overview_thumbnail_request (ephy_embed_shell_get_default (), request);
    return;
  }
  if (ephy_about_handler_handle_application_icon (request))
    return;
  if (!g_resources_get_info (path, 0, &size, NULL, &error)) {
    ephy_engine_uri_scheme_request_finish_error (request, error);
    return;
  }

  request_view = ephy_engine_uri_scheme_request_get_web_view (request);
  uri = ephy_engine_web_view_get_uri (request_view);
  if (!uri)
    uri = "";

  if (g_strcmp0 (path, "/org/gnome/epiphany/overview.js") == 0) {
    if (ephy_embed_shell_can_show_overview (ephy_embed_shell_get_default (), request_view) &&
        ephy_engine_web_view_is_current_page (request_view, "ephy-about:overview")) {
      stream = g_resources_open_stream (path, 0, &error);
      if (stream)
        ephy_engine_uri_scheme_request_finish (request, stream, size, "text/javascript");
      else
        ephy_engine_uri_scheme_request_finish_error (request, error);
    } else {
      error = g_error_new_literal (G_IO_ERROR, G_IO_ERROR_PERMISSION_DENIED, "Overview script request is not authorized");
      ephy_engine_uri_scheme_request_finish_error (request, error);
    }
    return;
  }

  /* ephy-resource:// requests bypass CORS in order to allow custom schemes to
   * access ephy-resource://. Accordingly, we need some custom security to
   * prevent websites from directly accessing ephy-resource://.
   *
   * We'll have to leave open /page-icons and /page-templates since they are
   * needed for our alternate HTML error pages.
   */
  if (g_str_has_prefix (uri, "ephy-resource:") ||
      g_str_has_prefix (path, "/org/gnome/epiphany/page-icons/") ||
      g_str_has_prefix (path, "/org/gnome/epiphany/page-templates/") ||
      (g_str_has_prefix (uri, "ephy-reader:") && g_str_has_prefix (path, "/org/gnome/epiphany/readability/")) ||
      (g_str_has_prefix (uri, EPHY_VIEW_SOURCE_SCHEME ":") && g_str_has_prefix (path, "/org/gnome/epiphany/highlightjs/"))) {
    stream = g_resources_open_stream (path, 0, &error);
    if (stream)
      ephy_engine_uri_scheme_request_finish (request, stream, size, NULL);
    else
      ephy_engine_uri_scheme_request_finish_error (request, error);
    return;
  }

  error = g_error_new (WEBKIT_NETWORK_ERROR, WEBKIT_NETWORK_ERROR_FAILED,
                       _("URI %s not authorized to access Epiphany resource %s"),
                       uri, path);
  ephy_engine_uri_scheme_request_finish_error (request, error);
}

static gboolean
is_private_profile_mode (EphyEmbedShell *shell)
{
  EphyEmbedShellPrivate *priv = ephy_embed_shell_get_instance_private (shell);

  return priv->mode == EPHY_EMBED_SHELL_MODE_PRIVATE ||
         priv->mode == EPHY_EMBED_SHELL_MODE_INCOGNITO ||
         priv->mode == EPHY_EMBED_SHELL_MODE_AUTOMATION;
}

gboolean
ephy_embed_shell_should_remember_passwords (EphyEmbedShell *shell)
{
  return !is_private_profile_mode (shell) && g_settings_get_boolean (EPHY_SETTINGS_WEB, EPHY_PREFS_WEB_REMEMBER_PASSWORDS);
}

static void
ephy_embed_shell_create_network_session_if_needed (EphyEmbedShell *shell)
{
  EphyEmbedShellPrivate *priv = ephy_embed_shell_get_instance_private (shell);

  if (priv->network_session)
    return;

  if (priv->mode == EPHY_EMBED_SHELL_MODE_AUTOMATION || priv->mode == EPHY_EMBED_SHELL_MODE_INCOGNITO)
    priv->network_session = ephy_engine_session_new (TRUE);
  else
    priv->network_session = g_object_ref (ephy_engine_session_get_default ());
}

static void
ephy_embed_shell_create_legacy_network_session_if_needed (EphyEmbedShell *shell)
{
  EphyEmbedShellPrivate *priv = ephy_embed_shell_get_instance_private (shell);

  if (priv->legacy_network_session)
    return;

  /* Only the remaining WebKit website-data/ITP UI needs this session. Creating
   * it during ordinary Brave startup opens an unrelated engine's data stores. */
  if (priv->mode == EPHY_EMBED_SHELL_MODE_AUTOMATION || priv->mode == EPHY_EMBED_SHELL_MODE_INCOGNITO)
    priv->legacy_network_session = webkit_network_session_new_ephemeral ();
  else
    priv->legacy_network_session = webkit_network_session_new (ephy_profile_dir (), ephy_cache_dir ());
  webkit_network_session_set_persistent_credential_storage_enabled (priv->legacy_network_session, FALSE);
  webkit_network_session_set_itp_enabled (priv->legacy_network_session,
                                          g_settings_get_boolean (EPHY_SETTINGS_WEB, EPHY_PREFS_WEB_ENABLE_ITP));
}

static void
download_started_cb (EphyEmbedShell *shell,
                     EphyEngineDownload *download)
{
  g_autoptr (EphyDownload) ephy_download = NULL;
  gboolean ephy_download_set;

  /* Is download locked down? */
  if (g_settings_get_boolean (EPHY_SETTINGS_LOCKDOWN, EPHY_PREFS_LOCKDOWN_SAVE_TO_DISK) ||
      ephy_embed_shell_get_mode (shell) == EPHY_EMBED_SHELL_MODE_KIOSK) {
    ephy_engine_download_cancel (download);
    return;
  }

  /* Only create an EphyDownload for the EphyEngineDownload if it doesn't exist yet.
   * This can happen when the download has been started automatically by CEF,
   * due to a context menu action or policy checker decision. Downloads started
   * explicitly by Epiphany are marked with ephy-download-set GObject data.
   */
  ephy_download_set = GPOINTER_TO_INT (g_object_get_data (G_OBJECT (download), "ephy-download-set"));
  if (ephy_download_set)
    return;

  ephy_download = ephy_download_new (download);
  ephy_downloads_manager_add_download (ephy_embed_shell_get_downloads_manager (shell), ephy_download);
}

static void
enable_itp_setting_changed_cb (GSettings      *settings,
                               char           *key,
                               EphyEmbedShell *shell)
{
  EphyEmbedShellPrivate *priv = ephy_embed_shell_get_instance_private (shell);

  if (priv->legacy_network_session)
    webkit_network_session_set_itp_enabled (priv->legacy_network_session,
                                            g_settings_get_boolean (EPHY_SETTINGS_WEB,
                                                                    EPHY_PREFS_WEB_ENABLE_ITP));
}

static void
ensure_profile_directory (const char *path)
{
  g_autoptr (GError) error = NULL;

  ephy_ensure_dir_exists (path, &error);
  if (error)
    g_error ("Failed to create directory %s: %s", path, error->message);
}

static void
ephy_embed_shell_startup (GApplication *application)
{
  EphyEmbedShell *shell = EPHY_EMBED_SHELL (application);
  EphyEmbedShellPrivate *priv = ephy_embed_shell_get_instance_private (shell);

  G_APPLICATION_CLASS (ephy_embed_shell_parent_class)->startup (application);

  ensure_profile_directory (ephy_profile_dir ());
  ensure_profile_directory (ephy_cache_dir ());
  ensure_profile_directory (ephy_config_dir ());

  priv->password_manager = ephy_password_manager_new ();

  if (!ephy_engine_session_is_ephemeral (priv->network_session)) {
    g_autofree char *favicon_dir = g_build_filename (ephy_cache_dir (), "favicons", NULL);
    ephy_engine_favicon_database_set_directory (ephy_engine_favicon_database_get_default (), favicon_dir);
  }

  /* about: URIs handler */
  priv->about_handler = ephy_about_handler_new ();
  ephy_engine_register_uri_scheme (EPHY_ABOUT_SCHEME,
                                   (EphyEngineSchemeRequestCallback)about_request_cb,
                                   shell, NULL);

  /* view source handler */
  priv->source_handler = ephy_view_source_handler_new ();
  ephy_engine_register_uri_scheme (EPHY_VIEW_SOURCE_SCHEME,
                                   (EphyEngineSchemeRequestCallback)source_request_cb,
                                   shell, NULL);

  /* reader mode handler */
  priv->reader_handler = ephy_reader_handler_new ();
  ephy_engine_register_uri_scheme (EPHY_READER_SCHEME,
                                   (EphyEngineSchemeRequestCallback)reader_request_cb,
                                   shell, NULL);

  /* ephy-resource handler */
  ephy_engine_register_uri_scheme ("ephy-resource",
                                   (EphyEngineSchemeRequestCallback)ephy_resource_request_cb,
                                   NULL, NULL);

  g_signal_connect_object (priv->network_session, "download-started",
                           G_CALLBACK (download_started_cb), shell, G_CONNECT_SWAPPED);

  g_signal_connect_object (EPHY_SETTINGS_WEB, "changed::enable-itp",
                           G_CALLBACK (enable_itp_setting_changed_cb), shell, G_CONNECT_DEFAULT);

}

static void
ephy_embed_shell_shutdown (GApplication *application)
{
  G_APPLICATION_CLASS (ephy_embed_shell_parent_class)->shutdown (application);

  ephy_embed_utils_shutdown ();
}

static void
ephy_embed_shell_set_property (GObject      *object,
                               guint         prop_id,
                               const GValue *value,
                               GParamSpec   *pspec)
{
  EphyEmbedShellPrivate *priv = ephy_embed_shell_get_instance_private (EPHY_EMBED_SHELL (object));

  switch ((EphyEmbedShellProps)prop_id) {
    case PROP_MODE:
      priv->mode = g_value_get_enum (value);
      break;
  }
}

static void
ephy_embed_shell_get_property (GObject    *object,
                               guint       prop_id,
                               GValue     *value,
                               GParamSpec *pspec)
{
  EphyEmbedShellPrivate *priv = ephy_embed_shell_get_instance_private (EPHY_EMBED_SHELL (object));

  switch ((EphyEmbedShellProps)prop_id) {
    case PROP_MODE:
      g_value_set_enum (value, priv->mode);
      break;
  }
}

static void
ephy_embed_shell_constructed (GObject *object)
{
  EphyEmbedShell *shell;
  EphyEmbedShellPrivate *priv;

  G_OBJECT_CLASS (ephy_embed_shell_parent_class)->constructed (object);

  shell = EPHY_EMBED_SHELL (object);
  priv = ephy_embed_shell_get_instance_private (shell);
  priv->guid = g_dbus_generate_guid ();

  priv->cancellable = g_cancellable_new ();

  ephy_embed_shell_create_network_session_if_needed (shell);

  if (priv->mode != EPHY_EMBED_SHELL_MODE_SEARCH_PROVIDER) {
    priv->permissions_manager = ephy_permissions_manager_new (priv->network_session);
  }

}

static void
ephy_embed_shell_init (EphyEmbedShell *shell)
{
  /* globally accessible singleton */
  g_assert (!embed_shell);
  embed_shell = shell;
}

static void
ephy_embed_shell_class_init (EphyEmbedShellClass *klass)
{
  GObjectClass *object_class = G_OBJECT_CLASS (klass);
  GApplicationClass *application_class = G_APPLICATION_CLASS (klass);

  object_class->dispose = ephy_embed_shell_dispose;
  object_class->set_property = ephy_embed_shell_set_property;
  object_class->get_property = ephy_embed_shell_get_property;
  object_class->constructed = ephy_embed_shell_constructed;

  application_class->startup = ephy_embed_shell_startup;
  application_class->shutdown = ephy_embed_shell_shutdown;

  object_properties[PROP_MODE] =
    g_param_spec_enum ("mode",
                       NULL, NULL,
                       EPHY_TYPE_EMBED_SHELL_MODE,
                       EPHY_EMBED_SHELL_MODE_BROWSER,
                       G_PARAM_READWRITE | G_PARAM_CONSTRUCT_ONLY | G_PARAM_STATIC_STRINGS);

  g_object_class_install_properties (object_class,
                                     G_N_ELEMENTS (object_properties),
                                     object_properties);

/**
 * EphyEmbedShell::finished-restoring-window:
 * @shell: the #EphyEmbedShell
 *
 * The ::finished-restoring-window signal is emitted when the
 * session finishes restoring a window.
 **/
  signals[RESTORED_WINDOW] =
    g_signal_new ("window-restored",
                  EPHY_TYPE_EMBED_SHELL,
                  G_SIGNAL_RUN_FIRST,
                  0, NULL, NULL, NULL,
                  G_TYPE_NONE,
                  0);

  /**
   * EphyEmbedShell::web-view-created:
   * @shell: the #EphyEmbedShell
   * @view: the newly created #EphyWebView
   *
   * The ::web-view-created signal will be emitted every time a new
   * #EphyWebView is created.
   *
   **/
  signals[WEB_VIEW_CREATED] =
    g_signal_new ("web-view-created",
                  EPHY_TYPE_EMBED_SHELL,
                  G_SIGNAL_RUN_LAST,
                  0, NULL, NULL, NULL,
                  G_TYPE_NONE, 1,
                  EPHY_TYPE_WEB_VIEW);

  /**
   * EphyEmbedShell::password-form-focused
   * @shell: the #EphyEmbedShell
   * @page_id: the identifier of the web page
   * @insecure_action: whether the target of the form is http://
   *
   * Emitted when a form in a web page gains focus.
   */
  signals[PASSWORD_FORM_FOCUSED] =
    g_signal_new ("password-form-focused",
                  EPHY_TYPE_EMBED_SHELL,
                  G_SIGNAL_RUN_FIRST,
                  0, NULL, NULL, NULL,
                  G_TYPE_NONE, 2,
                  G_TYPE_UINT64,
                  G_TYPE_BOOLEAN);

  /**
   * EphyEmbedShell::password-form-submitted:
   * @shell: the #EphyEmbedShell
   * @data: An #EphyPasswordRequestData containing information regarding the password
   *
   * Emitted when a Password Save request has been received, and the UI should
   * provide the user with a prompt of some form
   */
  signals[PASSWORD_FORM_SUBMITTED] =
    g_signal_new ("password-form-submitted",
                  EPHY_TYPE_EMBED_SHELL,
                  G_SIGNAL_RUN_FIRST,
                  0, NULL, NULL, NULL,
                  G_TYPE_NONE, 1,
                  G_TYPE_POINTER);

  /**
   * EphyEmbedShell::autofill:
   * @shell: the #EphyEmbedShell
   * @page_id: the identifier of the web page created
   * @frame_id: the identifier of the web frame
   * @css_selector: css selector of input element
   * @is_fillable_element: is input element fillable
   * @has_personal_fields: does input element's form has personal fields
   * @has_card_fields: does input element's form has credit card fields
   * @element_x: x position of input element
   * @element_y: y position of input element
   * @element_width: width on input element
   * @element_height: height of input element
   *
   * Emitted when the user double clicks on:
   * 1. A fillable input element
   * 2. An input element where its form has fillable fields
   */
  signals[AUTOFILL_SIGNAL] =
    g_signal_new ("autofill",
                  EPHY_TYPE_EMBED_SHELL,
                  G_SIGNAL_RUN_FIRST,
                  0, NULL, NULL, NULL,
                  G_TYPE_NONE, 10,
                  G_TYPE_UINT64,
                  G_TYPE_UINT64,
                  G_TYPE_STRING,
                  G_TYPE_BOOLEAN,
                  G_TYPE_BOOLEAN,
                  G_TYPE_BOOLEAN,
                  G_TYPE_UINT64,
                  G_TYPE_UINT64,
                  G_TYPE_UINT64,
                  G_TYPE_UINT64);
}

/**
 * ephy_embed_shell_get_default:
 *
 * Retrieves the default #EphyEmbedShell object
 *
 * Return value: (transfer none): the default #EphyEmbedShell
 **/
EphyEmbedShell *
ephy_embed_shell_get_default (void)
{
  return embed_shell;
}

void
ephy_embed_shell_set_page_setup (EphyEmbedShell *shell,
                                 GtkPageSetup   *page_setup)
{
  EphyEmbedShellPrivate *priv = ephy_embed_shell_get_instance_private (shell);
  g_autofree char *path = NULL;

  g_assert (EPHY_IS_EMBED_SHELL (shell));

  if (page_setup)
    g_object_ref (page_setup);
  else
    page_setup = gtk_page_setup_new ();

  if (priv->page_setup)
    g_object_unref (priv->page_setup);

  priv->page_setup = page_setup;

  path = g_build_filename (ephy_profile_dir (), PAGE_SETUP_FILENAME, NULL);
  gtk_page_setup_to_file (page_setup, path, NULL);
}

/**
 * ephy_embed_shell_get_page_setup:
 *
 * Return value: (transfer none):
 **/
GtkPageSetup *
ephy_embed_shell_get_page_setup (EphyEmbedShell *shell)
{
  EphyEmbedShellPrivate *priv = ephy_embed_shell_get_instance_private (shell);

  g_assert (EPHY_IS_EMBED_SHELL (shell));

  if (!priv->page_setup) {
    g_autofree char *path = NULL;

    path = g_build_filename (ephy_profile_dir (), PAGE_SETUP_FILENAME, NULL);
    priv->page_setup = gtk_page_setup_new_from_file (path, NULL);

    /* If that still didn't work, create a new, empty one */
    if (!priv->page_setup)
      priv->page_setup = gtk_page_setup_new ();
  }

  return priv->page_setup;
}

/**
 * ephy_embed_shell_set_print_settings:
 * @shell: the #EphyEmbedShell
 * @settings: the new #GtkPrintSettings object
 *
 * Sets the global #GtkPrintSettings object.
 *
 **/
void
ephy_embed_shell_set_print_settings (EphyEmbedShell   *shell,
                                     GtkPrintSettings *settings)
{
  EphyEmbedShellPrivate *priv = ephy_embed_shell_get_instance_private (shell);
  g_autofree char *path = NULL;

  g_assert (EPHY_IS_EMBED_SHELL (shell));

  if (settings)
    g_object_ref (settings);

  if (priv->print_settings)
    g_object_unref (priv->print_settings);

  priv->print_settings = settings ? settings : gtk_print_settings_new ();

  path = g_build_filename (ephy_profile_dir (), PRINT_SETTINGS_FILENAME, NULL);
  gtk_print_settings_to_file (settings, path, NULL);
}

/**
 * ephy_embed_shell_get_print_settings:
 * @shell: the #EphyEmbedShell
 *
 * Gets the global #GtkPrintSettings object.
 *
 * Returns: (transfer none): a #GtkPrintSettings object
 **/
GtkPrintSettings *
ephy_embed_shell_get_print_settings (EphyEmbedShell *shell)
{
  EphyEmbedShellPrivate *priv = ephy_embed_shell_get_instance_private (shell);

  g_assert (EPHY_IS_EMBED_SHELL (shell));

  if (!priv->print_settings) {
    g_autofree char *path = NULL;

    path = g_build_filename (ephy_profile_dir (), PRINT_SETTINGS_FILENAME, NULL);
    priv->print_settings = gtk_print_settings_new_from_file (path, NULL);

    /* Note: the gtk print settings file format is the same as our
     * legacy one, so no need to migrate here.
     */

    if (!priv->print_settings)
      priv->print_settings = gtk_print_settings_new ();
  }

  return priv->print_settings;
}

/**
 * ephy_embed_shell_get_mode:
 * @shell: an #EphyEmbedShell
 *
 * Returns: the global mode of the @shell
 **/
EphyEmbedShellMode
ephy_embed_shell_get_mode (EphyEmbedShell *shell)
{
  EphyEmbedShellPrivate *priv = ephy_embed_shell_get_instance_private (shell);

  g_assert (EPHY_IS_EMBED_SHELL (shell));

  return priv->mode;
}

const char *
ephy_embed_shell_get_guid (EphyEmbedShell *shell)
{
  EphyEmbedShellPrivate *priv = ephy_embed_shell_get_instance_private (shell);

  return priv->guid;
}

EphyEngineSession *
ephy_embed_shell_get_network_session (EphyEmbedShell *shell)
{
  EphyEmbedShellPrivate *priv = ephy_embed_shell_get_instance_private (shell);

  return priv->network_session;
}

EphyDownloadsManager *
ephy_embed_shell_get_downloads_manager (EphyEmbedShell *shell)
{
  EphyEmbedShellPrivate *priv = ephy_embed_shell_get_instance_private (shell);

  if (!priv->downloads_manager)
    priv->downloads_manager = EPHY_DOWNLOADS_MANAGER (g_object_new (EPHY_TYPE_DOWNLOADS_MANAGER, NULL));
  return priv->downloads_manager;
}

EphyPermissionsManager *
ephy_embed_shell_get_permissions_manager (EphyEmbedShell *shell)
{
  EphyEmbedShellPrivate *priv = ephy_embed_shell_get_instance_private (shell);

  return priv->permissions_manager;
}

EphySearchEngineManager *
ephy_embed_shell_get_search_engine_manager (EphyEmbedShell *shell)
{
  EphyEmbedShellPrivate *priv = ephy_embed_shell_get_instance_private (shell);

  if (!priv->search_engine_manager)
    priv->search_engine_manager = ephy_search_engine_manager_new ();
  return priv->search_engine_manager;
}

EphyPasswordManager *
ephy_embed_shell_get_password_manager (EphyEmbedShell *shell)
{
  EphyEmbedShellPrivate *priv = ephy_embed_shell_get_instance_private (shell);

  return priv->password_manager;
}

EphyEngineFaviconDatabase *
ephy_embed_shell_get_favicon_database (EphyEmbedShell *shell)
{
  return ephy_engine_favicon_database_get_default ();
}

void
ephy_embed_shell_register_ucm (EphyEmbedShell           *shell,
                               WebKitUserContentManager *ucm)
{
  EphyEmbedShellPrivate *priv = ephy_embed_shell_get_instance_private (shell);

  /* Warning: message handlers connected here should use the private script
   * world priv->guid to make them unavailable to web content. If a message
   * handler needs to be temporarily accessible to web content in order to
   * implement internal Epiphany pages, register it using
   * ephy_web_view_register_message_handler() to ensure it is unregistered
   * before any subsequent page load.
   */

  webkit_user_content_manager_register_script_message_handler (ucm,
                                                               "passwordFormFocused",
                                                               priv->guid);
  g_signal_connect_object (ucm, "script-message-received::passwordFormFocused",
                           G_CALLBACK (web_process_extension_password_form_focused_message_received_cb),
                           shell, G_CONNECT_DEFAULT);

  webkit_user_content_manager_register_script_message_handler (ucm,
                                                               "passwordManagerSave",
                                                               priv->guid);
  g_signal_connect_object (ucm, "script-message-received::passwordManagerSave",
                           G_CALLBACK (web_process_extension_password_manager_save_received_cb),
                           shell, G_CONNECT_DEFAULT);

  webkit_user_content_manager_register_script_message_handler (ucm,
                                                               "passwordManagerRequestSave",
                                                               priv->guid);
  g_signal_connect_object (ucm, "script-message-received::passwordManagerRequestSave",
                           G_CALLBACK (web_process_extension_password_manager_request_save_received_cb),
                           shell, G_CONNECT_DEFAULT);

  webkit_user_content_manager_register_script_message_handler (ucm,
                                                               "autofillAskUser",
                                                               priv->guid);
  g_signal_connect_object (ucm, "script-message-received::autofillAskUser",
                           G_CALLBACK (web_process_extension_autofill_askuser_received_cb),
                           shell, G_CONNECT_DEFAULT);

  /* User Scripts */
  ephy_embed_prefs_apply_user_style (ucm);
  ephy_embed_prefs_apply_user_javascript (ucm);
}

void
ephy_embed_shell_unregister_ucm (EphyEmbedShell           *shell,
                                 WebKitUserContentManager *ucm)
{
  EphyEmbedShellPrivate *priv = ephy_embed_shell_get_instance_private (shell);

  webkit_user_content_manager_unregister_script_message_handler (ucm,
                                                                 "passwordFormFocused",
                                                                 priv->guid);
  webkit_user_content_manager_unregister_script_message_handler (ucm,
                                                                 "passwordManagerSave",
                                                                 priv->guid);
  webkit_user_content_manager_unregister_script_message_handler (ucm,
                                                                 "passwordManagerRequestSave",
                                                                 priv->guid);
  webkit_user_content_manager_unregister_script_message_handler (ucm,
                                                                 "autofillAskUser",
                                                                 priv->guid);
}

/* Website-data management is the remaining legacy network-session service. */
WebKitWebsiteDataManager *
ephy_embed_shell_get_website_data_manager (EphyEmbedShell *shell)
{
  EphyEmbedShellPrivate *priv = ephy_embed_shell_get_instance_private (shell);

  ephy_embed_shell_create_legacy_network_session_if_needed (shell);
  return webkit_network_session_get_website_data_manager (priv->legacy_network_session);
}

EphyEngineDownload *
ephy_embed_shell_download_uri (EphyEmbedShell *shell, const char *uri)
{
  EphyEmbedShellPrivate *priv = ephy_embed_shell_get_instance_private (shell);
  return ephy_engine_session_download_uri (priv->network_session, uri);
}
