/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "config.h"
#include "ephy-web-app-dialog.h"
#include "ephy-engine-extensions.h"
#include "ephy-prefs.h"
#include "ephy-settings.h"
#include "web-app-config.h"
#include <glib/gi18n.h>
#include <stdlib.h>

struct _EphyWebAppDialog {
  AdwPreferencesDialog parent_instance;
  GtkWidget *name, *url, *preview, *error, *save, *open_links, *domains, *domain_entry, *domain_error;
  GIcon *icon, *site_icon;
  GKeyFile *config;
  GPtrArray *extensions;
  GCancellable *cancellable;
};
G_DEFINE_FINAL_TYPE (EphyWebAppDialog, ephy_web_app_dialog, ADW_TYPE_PREFERENCES_DIALOG)

static void remove_domain (GtkButton *button, EphyWebAppDialog *self) {
  gboolean focused = gtk_widget_has_focus (GTK_WIDGET (button));
  adw_wrap_box_remove (ADW_WRAP_BOX (self->domains), GTK_WIDGET (button));
  gtk_widget_set_visible (self->domains, gtk_widget_get_first_child (self->domains) != NULL);
  if (focused) gtk_widget_grab_focus (self->domain_entry);
}
static void append_domain (EphyWebAppDialog *self, const char *domain) {
  g_autofree char *normalized = gnosis_web_app_link_domain (domain);
  GtkWidget *chip = gtk_button_new ();
  GtkWidget *content = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 6);
  GtkWidget *label = gtk_label_new (normalized ? normalized : domain);
  g_autofree char *remove = g_strdup_printf (_("Remove %s"), normalized ? normalized : domain);
  gtk_label_set_ellipsize (GTK_LABEL (label), PANGO_ELLIPSIZE_MIDDLE);
  gtk_label_set_max_width_chars (GTK_LABEL (label), 28);
  gtk_box_append (GTK_BOX (content), label);
  gtk_box_append (GTK_BOX (content), gtk_image_new_from_icon_name ("window-close-symbolic"));
  gtk_button_set_child (GTK_BUTTON (chip), content);
  gtk_widget_add_css_class (chip, "pill");
  gtk_widget_set_tooltip_text (chip, remove);
  gtk_accessible_update_property (GTK_ACCESSIBLE (chip), GTK_ACCESSIBLE_PROPERTY_LABEL, remove, -1);
  g_object_set_data_full (G_OBJECT (chip), "domain", g_strdup (normalized ? normalized : domain), g_free);
  g_signal_connect_object (chip, "clicked", G_CALLBACK (remove_domain), self, 0);
  adw_wrap_box_append (ADW_WRAP_BOX (self->domains), chip);
  gtk_widget_set_visible (self->domains, TRUE);
}
static gboolean add_domain (EphyWebAppDialog *self) {
  const char *value = gtk_editable_get_text (GTK_EDITABLE (self->domain_entry));
  g_autofree char *domain = NULL;
  const char *message = NULL;
  guint count = 0;
  if (!*value) return TRUE;
  domain = gnosis_web_app_link_domain (value);
  if (!domain) message = _("Enter a domain such as example.com.");
  for (GtkWidget *chip = gtk_widget_get_first_child (self->domains); chip; chip = gtk_widget_get_next_sibling (chip)) {
    if (domain && !g_strcmp0 (domain, g_object_get_data (G_OBJECT (chip), "domain"))) goto added;
    count++;
  }
  if (!message && count >= 64) message = _("You can add up to 64 domains.");
  if (message) {
    gtk_widget_add_css_class (self->domain_entry, "error");
    gtk_label_set_text (GTK_LABEL (self->domain_error), message);
    gtk_widget_set_visible (self->domain_error, TRUE);
    gtk_widget_grab_focus (self->domain_entry);
    return FALSE;
  }
  append_domain (self, domain);
added:
  gtk_editable_set_text (GTK_EDITABLE (self->domain_entry), "");
  gtk_widget_remove_css_class (self->domain_entry, "error");
  gtk_widget_set_visible (self->domain_error, FALSE);
  return TRUE;
}
static void add_domain_clicked (GtkWidget *widget, EphyWebAppDialog *self) {
  if (add_domain (self)) gtk_widget_grab_focus (self->domain_entry);
}

static void set_icon (EphyWebAppDialog *self, GIcon *icon) {
  g_object_set_data (G_OBJECT (self), "validated-icon", NULL);
  g_set_object (&self->icon, icon);
  gtk_image_set_from_gicon (GTK_IMAGE (self->preview), icon);
}

static gboolean filter_icon (gpointer item, gpointer data) {
  const char *name = gtk_string_object_get_string (item);
  const char *search = gtk_editable_get_text (GTK_EDITABLE (data));
  g_autofree char *folded_name = g_utf8_casefold (name, -1);
  g_autofree char *folded_search = g_utf8_casefold (search, -1);
  return strstr (folded_name, folded_search) != NULL;
}
static void search_icons (GtkSearchEntry *entry, GtkFilter *filter) {
  gtk_filter_changed (filter, GTK_FILTER_CHANGE_DIFFERENT);
}
static void setup_icon (GtkSignalListItemFactory *factory, GtkListItem *item, gpointer data) {
  GtkWidget *box = gtk_box_new (GTK_ORIENTATION_VERTICAL, 6);
  GtkWidget *image = gtk_image_new ();
  GtkWidget *label = gtk_label_new (NULL);
  gtk_image_set_pixel_size (GTK_IMAGE (image), 40);
  gtk_widget_set_size_request (box, 76, 80);
  gtk_widget_set_margin_top (box, 8);
  gtk_widget_set_margin_bottom (box, 8);
  gtk_label_set_max_width_chars (GTK_LABEL (label), 11);
  gtk_label_set_ellipsize (GTK_LABEL (label), PANGO_ELLIPSIZE_MIDDLE);
  gtk_widget_add_css_class (label, "caption");
  gtk_box_append (GTK_BOX (box), image);
  gtk_box_append (GTK_BOX (box), label);
  gtk_list_item_set_child (item, box);
}
static void bind_icon (GtkSignalListItemFactory *factory, GtkListItem *item, gpointer data) {
  const char *name = gtk_string_object_get_string (gtk_list_item_get_item (item));
  GtkWidget *box = gtk_list_item_get_child (item);
  GtkWidget *image = gtk_widget_get_first_child (box);
  gtk_image_set_from_icon_name (GTK_IMAGE (image), name);
  gtk_label_set_text (GTK_LABEL (gtk_widget_get_next_sibling (image)), name);
  gtk_widget_set_tooltip_text (box, name);
}
static void activate_icon (GtkGridView *grid, guint position, EphyWebAppDialog *self) {
  g_autoptr (GtkStringObject) item = g_list_model_get_item (G_LIST_MODEL (gtk_grid_view_get_model (grid)), position);
  g_autoptr (GIcon) icon = g_themed_icon_new (gtk_string_object_get_string (item));
  AdwDialog *chooser = g_object_get_data (G_OBJECT (grid), "chooser");
  set_icon (self, icon);
  adw_dialog_close (chooser);
}
static int compare_icons (const void *a, const void *b) {
  return g_strcmp0 (*(const char *const *)a, *(const char *const *)b);
}
static void choose_theme_icon (GtkWidget *widget, const char *action_name, GVariant *parameter) {
  EphyWebAppDialog *self = EPHY_WEB_APP_DIALOG (widget);
  GtkIconTheme *theme = gtk_icon_theme_get_for_display (gtk_widget_get_display (GTK_WIDGET (self)));
  g_auto (GStrv) names = gtk_icon_theme_get_icon_names (theme);
  AdwDialog *chooser = adw_dialog_new ();
  GtkWidget *toolbar = adw_toolbar_view_new ();
  GtkWidget *box = gtk_box_new (GTK_ORIENTATION_VERTICAL, 12);
  GtkWidget *search = gtk_search_entry_new ();
  GtkWidget *scroll = gtk_scrolled_window_new ();
  GtkStringList *strings;
  GtkCustomFilter *filter;
  GtkFilterListModel *filtered;
  GtkSingleSelection *selection;
  GtkListItemFactory *factory;
  GtkWidget *grid;

  qsort (names, g_strv_length (names), sizeof (char *), compare_icons);
  strings = gtk_string_list_new ((const char *const *)names);
  filter = gtk_custom_filter_new (filter_icon, g_object_ref (search), g_object_unref);
  filtered = gtk_filter_list_model_new (G_LIST_MODEL (strings), GTK_FILTER (filter));
  selection = gtk_single_selection_new (G_LIST_MODEL (filtered));
  gtk_single_selection_set_autoselect (selection, FALSE);
  factory = gtk_signal_list_item_factory_new ();
  g_signal_connect (factory, "setup", G_CALLBACK (setup_icon), NULL);
  g_signal_connect (factory, "bind", G_CALLBACK (bind_icon), NULL);
  grid = gtk_grid_view_new (GTK_SELECTION_MODEL (selection), factory);
  gtk_grid_view_set_min_columns (GTK_GRID_VIEW (grid), 4);
  gtk_grid_view_set_max_columns (GTK_GRID_VIEW (grid), 7);
  gtk_grid_view_set_single_click_activate (GTK_GRID_VIEW (grid), TRUE);
  g_object_set_data (G_OBJECT (grid), "chooser", chooser);
  g_signal_connect_object (grid, "activate", G_CALLBACK (activate_icon), self, 0);
  g_signal_connect_object (search, "search-changed", G_CALLBACK (search_icons), filter, 0);
  gtk_widget_set_margin_start (box, 12);
  gtk_widget_set_margin_end (box, 12);
  gtk_widget_set_margin_bottom (box, 12);
  gtk_widget_set_vexpand (scroll, TRUE);
  gtk_scrolled_window_set_policy (GTK_SCROLLED_WINDOW (scroll), GTK_POLICY_NEVER, GTK_POLICY_AUTOMATIC);
  gtk_scrolled_window_set_child (GTK_SCROLLED_WINDOW (scroll), grid);
  gtk_box_append (GTK_BOX (box), search);
  gtk_box_append (GTK_BOX (box), scroll);
  adw_toolbar_view_add_top_bar (ADW_TOOLBAR_VIEW (toolbar), adw_header_bar_new ());
  adw_toolbar_view_set_content (ADW_TOOLBAR_VIEW (toolbar), box);
  adw_dialog_set_title (ADW_DIALOG (chooser), _("Choose a Theme Icon"));
  adw_dialog_set_content_width (ADW_DIALOG (chooser), 560);
  adw_dialog_set_content_height (ADW_DIALOG (chooser), 520);
  adw_dialog_set_child (ADW_DIALOG (chooser), toolbar);
  adw_dialog_present (ADW_DIALOG (chooser), GTK_WIDGET (self));
  gtk_widget_grab_focus (search);
}
static void file_chosen (GtkFileDialog *dialog, GAsyncResult *result, EphyWebAppDialog *self) {
  g_autoptr (GError) error = NULL;
  g_autoptr (GFile) file = gtk_file_dialog_open_finish (dialog, result, &error);
  if (file && !g_cancellable_is_cancelled (self->cancellable)) {
    g_autoptr (GIcon) icon = g_file_icon_new (file);
    set_icon (self, icon);
  }
  g_object_unref (self);
}
static void choose_file_icon (GtkWidget *widget, const char *action_name, GVariant *parameter) {
  EphyWebAppDialog *self = EPHY_WEB_APP_DIALOG (widget);
  g_autoptr (GtkFileDialog) dialog = gtk_file_dialog_new ();
  g_autoptr (GtkFileFilter) filter = gtk_file_filter_new ();
  g_autoptr (GListStore) filters = g_list_store_new (GTK_TYPE_FILE_FILTER);
  gtk_file_filter_set_name (filter, _("Images"));
  gtk_file_filter_add_mime_type (filter, "image/*");
  g_list_store_append (filters, filter);
  gtk_file_dialog_set_title (dialog, _("Choose a Web App Icon"));
  gtk_file_dialog_set_filters (dialog, G_LIST_MODEL (filters));
  gtk_file_dialog_open (dialog, GTK_WINDOW (gtk_widget_get_root (GTK_WIDGET (self))),
                        self->cancellable, (GAsyncReadyCallback)file_chosen, g_object_ref (self));
}
static void restore_site_icon (GtkWidget *widget, const char *action_name, GVariant *parameter) {
  EphyWebAppDialog *self = EPHY_WEB_APP_DIALOG (widget);
  set_icon (self, self->site_icon);
}

GBytes *ephy_web_app_dialog_dup_icon_bytes (EphyWebAppDialog *self, GError **error) {
  g_autoptr (GIcon) loadable = NULL;
  g_autoptr (GInputStream) stream = NULL;
  g_autoptr (GdkPixbuf) pixbuf = NULL;
  g_autofree char *buffer = NULL;
  gsize length = 0;
  GBytes *validated = g_object_get_data (G_OBJECT (self), "validated-icon");
  if (validated) return g_bytes_ref (validated);
  if (G_IS_THEMED_ICON (self->icon)) {
    GtkIconTheme *theme = gtk_icon_theme_get_for_display (gtk_widget_get_display (GTK_WIDGET (self)));
    const char *name = g_themed_icon_get_names (G_THEMED_ICON (self->icon))[0];
    g_autoptr (GtkIconPaintable) paintable = gtk_icon_theme_lookup_icon (theme, name, NULL, 256, 1, GTK_TEXT_DIR_NONE, 0);
    GFile *file = gtk_icon_paintable_get_file (paintable);
    if (file) loadable = g_file_icon_new (file);
  } else loadable = g_object_ref (self->icon);
  if (!G_IS_LOADABLE_ICON (loadable)) {
    g_set_error_literal (error, G_IO_ERROR, G_IO_ERROR_NOT_SUPPORTED, _("This icon could not be loaded.")); return NULL;
  }
  stream = g_loadable_icon_load (G_LOADABLE_ICON (loadable), 256, NULL, self->cancellable, error);
  if (!stream) return NULL;
  pixbuf = gdk_pixbuf_new_from_stream_at_scale (stream, 256, 256, TRUE, self->cancellable, error);
  if (!pixbuf || !gdk_pixbuf_save_to_buffer (pixbuf, &buffer, &length, "png", error, NULL)) return NULL;
  return g_bytes_new_take (g_steal_pointer (&buffer), length);
}
static void accept_setup (GtkButton *button, EphyWebAppDialog *self) {
  g_autoptr (GError) error = NULL;
  g_autoptr (GBytes) bytes = ephy_web_app_dialog_dup_icon_bytes (self, &error);
  g_autoptr (GUri) uri = g_uri_parse (ephy_web_app_dialog_get_url (self), G_URI_FLAGS_NONE, NULL);
  const char *scheme = uri ? g_uri_get_scheme (uri) : NULL;
  if (!add_domain (self)) return;
  if (!*ephy_web_app_dialog_get_name (self) || !scheme ||
      !(g_str_equal (scheme, "http") || g_str_equal (scheme, "https")) || !g_uri_get_host (uri)) {
    gtk_label_set_text (GTK_LABEL (self->error), _("Enter a name and an HTTP or HTTPS address.")); return;
  }
  if (!bytes) { gtk_label_set_text (GTK_LABEL (self->error), error->message); return; }
  {
    g_autoptr (GKeyFile) config = ephy_web_app_dialog_dup_config (self);
    if (!gnosis_web_app_config_validate (config, &error)) {
      gtk_label_set_text (GTK_LABEL (self->error), error->message); return;
    }
  }
  g_object_set_data_full (G_OBJECT (self), "validated-icon", g_bytes_ref (bytes), (GDestroyNotify)g_bytes_unref);
  g_signal_emit_by_name (self, "accepted");
}
static void closed_setup (AdwDialog *dialog, EphyWebAppDialog *self) {
  if (self->cancellable) g_cancellable_cancel (self->cancellable);
}
static void dispose (GObject *object) {
  EphyWebAppDialog *self = EPHY_WEB_APP_DIALOG (object);
  if (self->cancellable) g_cancellable_cancel (self->cancellable);
  g_clear_object (&self->cancellable);
  g_clear_object (&self->icon);
  g_clear_object (&self->site_icon);
  g_clear_pointer (&self->config, g_key_file_unref);
  g_clear_pointer (&self->extensions, g_ptr_array_unref);
  G_OBJECT_CLASS (ephy_web_app_dialog_parent_class)->dispose (object);
}
static void ephy_web_app_dialog_class_init (EphyWebAppDialogClass *klass) {
  G_OBJECT_CLASS (klass)->dispose = dispose;
  gtk_widget_class_install_action (GTK_WIDGET_CLASS (klass), "webapp.choose-theme-icon", NULL, choose_theme_icon);
  gtk_widget_class_install_action (GTK_WIDGET_CLASS (klass), "webapp.choose-file-icon", NULL, choose_file_icon);
  gtk_widget_class_install_action (GTK_WIDGET_CLASS (klass), "webapp.restore-site-icon", NULL, restore_site_icon);
  g_signal_new ("accepted", G_TYPE_FROM_CLASS (klass), G_SIGNAL_RUN_LAST, 0, NULL, NULL, NULL, G_TYPE_NONE, 0);
}
static void ephy_web_app_dialog_init (EphyWebAppDialog *self) {
  self->extensions = g_ptr_array_new ();
  self->cancellable = g_cancellable_new ();
  adw_preferences_dialog_set_search_enabled (ADW_PREFERENCES_DIALOG (self), FALSE);
  adw_dialog_set_content_width (ADW_DIALOG (self), 520);
  adw_dialog_set_content_height (ADW_DIALOG (self), 620);
  g_signal_connect (self, "closed", G_CALLBACK (closed_setup), self);
}
static void add_extension (EphyWebAppDialog *self, AdwPreferencesGroup *group,
                           const char *id, const char *name, const char *path, gboolean default_enabled) {
  GtkWidget *row = adw_switch_row_new ();
  gboolean configured = g_key_file_has_group (self->config, "Extensions");
  adw_preferences_row_set_title (ADW_PREFERENCES_ROW (row), name);
  adw_preferences_row_set_use_markup (ADW_PREFERENCES_ROW (row), FALSE);
  adw_switch_row_set_active (ADW_SWITCH_ROW (row), configured ? gnosis_web_app_extension_enabled (self->config, id) : default_enabled);
  if (path && *path && !g_file_test (path, G_FILE_TEST_IS_DIR))
    adw_action_row_set_subtitle (ADW_ACTION_ROW (row), _("Unavailable — reinstall this extension to use it"));
  g_object_set_data_full (G_OBJECT (row), "extension-id", g_strdup (id), g_free);
  g_object_set_data_full (G_OBJECT (row), "extension-path", g_strdup (path), g_free);
  g_ptr_array_add (self->extensions, row);
  adw_preferences_group_add (group, row);
}
GtkWidget *ephy_web_app_dialog_new (const char *name, const char *url, GIcon *icon, GKeyFile *config, gboolean installing) {
  EphyWebAppDialog *self = g_object_new (EPHY_TYPE_WEB_APP_DIALOG, NULL);
  AdwPreferencesPage *page = ADW_PREFERENCES_PAGE (adw_preferences_page_new ());
  AdwPreferencesGroup *details = ADW_PREFERENCES_GROUP (adw_preferences_group_new ());
  AdwPreferencesGroup *extensions = ADW_PREFERENCES_GROUP (adw_preferences_group_new ());
  AdwPreferencesGroup *links = ADW_PREFERENCES_GROUP (adw_preferences_group_new ());
  GtkWidget *row = adw_action_row_new ();
  GtkWidget *menu = gtk_menu_button_new ();
  g_autoptr (GMenu) choices = g_menu_new ();
  AdwPreferencesGroup *actions = ADW_PREFERENCES_GROUP (adw_preferences_group_new ());
  g_autoptr (JsonArray) inventory = ephy_engine_extensions_dup_native_snapshot ();
  g_autoptr (GHashTable) ids = g_hash_table_new_full (g_str_hash, g_str_equal, g_free, NULL);
  g_auto (GStrv) saved = NULL;

  self->config = config ? g_key_file_ref (config) : g_key_file_new ();
  self->site_icon = g_object_ref (icon);
  adw_dialog_set_title (ADW_DIALOG (self), installing ? _("Add Web App") : _("Edit Web App"));
  self->name = adw_entry_row_new ();
  self->url = adw_entry_row_new ();
  adw_preferences_row_set_title (ADW_PREFERENCES_ROW (self->name), _("Name"));
  adw_preferences_row_set_title (ADW_PREFERENCES_ROW (self->url), _("Address"));
  gtk_editable_set_text (GTK_EDITABLE (self->name), name);
  gtk_editable_set_text (GTK_EDITABLE (self->url), url);
  /* Keep the site's address fixed while confirming a new installation. */
  gtk_editable_set_editable (GTK_EDITABLE (self->url), !installing);
  self->preview = gtk_image_new ();
  gtk_image_set_pixel_size (GTK_IMAGE (self->preview), 40);
  adw_action_row_add_prefix (ADW_ACTION_ROW (row), self->preview);
  adw_preferences_row_set_title (ADW_PREFERENCES_ROW (row), _("Icon"));
  gtk_menu_button_set_label (GTK_MENU_BUTTON (menu), _("Choose…"));
  gtk_widget_set_valign (menu, GTK_ALIGN_CENTER);
  adw_action_row_add_suffix (ADW_ACTION_ROW (row), menu);
  g_menu_append (choices, _("Choose from Icon Theme…"), "webapp.choose-theme-icon");
  g_menu_append (choices, _("Choose Image…"), "webapp.choose-file-icon");
  g_menu_append (choices, installing ? _("Use Website Icon") : _("Reset Icon"), "webapp.restore-site-icon");
  gtk_menu_button_set_menu_model (GTK_MENU_BUTTON (menu), G_MENU_MODEL (choices));
  set_icon (self, icon);
  adw_preferences_group_add (details, row);
  adw_preferences_group_add (details, self->name);
  adw_preferences_group_add (details, self->url);
  adw_preferences_page_add (page, details);
  adw_preferences_group_set_title (links, _("Link Associations"));
  adw_preferences_group_set_description (links, _("When Gnosis is your default browser, open links to these domains and their subdomains in this app."));
  self->open_links = adw_switch_row_new ();
  adw_preferences_row_set_title (ADW_PREFERENCES_ROW (self->open_links), _("Open Links in This App"));
  adw_switch_row_set_active (ADW_SWITCH_ROW (self->open_links), g_key_file_get_boolean (self->config, "Links", "enabled", NULL));
  self->domains = adw_wrap_box_new ();
  adw_wrap_box_set_child_spacing (ADW_WRAP_BOX (self->domains), 6);
  adw_wrap_box_set_line_spacing (ADW_WRAP_BOX (self->domains), 6);
  gtk_widget_set_margin_top (self->domains, 12);
  gtk_widget_set_visible (self->domains, FALSE);
  gtk_accessible_update_property (GTK_ACCESSIBLE (self->domains), GTK_ACCESSIBLE_PROPERTY_LABEL, _("Domains"), -1);
  self->domain_entry = adw_entry_row_new ();
  adw_preferences_row_set_title (ADW_PREFERENCES_ROW (self->domain_entry), _("Add Domain"));
  {
    GtkWidget *add = gtk_button_new_from_icon_name ("list-add-symbolic");
    gtk_widget_add_css_class (add, "flat");
    gtk_widget_set_valign (add, GTK_ALIGN_CENTER);
    gtk_widget_set_tooltip_text (add, _("Add Domain"));
    adw_entry_row_add_suffix (ADW_ENTRY_ROW (self->domain_entry), add);
    g_signal_connect_object (add, "clicked", G_CALLBACK (add_domain_clicked), self, 0);
    g_signal_connect_object (self->domain_entry, "entry-activated", G_CALLBACK (add_domain_clicked), self, 0);
  }
  self->domain_error = gtk_label_new (NULL);
  gtk_label_set_wrap (GTK_LABEL (self->domain_error), TRUE);
  gtk_label_set_xalign (GTK_LABEL (self->domain_error), 0);
  gtk_widget_add_css_class (self->domain_error, "error");
  gtk_widget_set_visible (self->domain_error, FALSE);
  {
    g_auto (GStrv) domains = g_key_file_get_string_list (self->config, "Links", "domains", NULL, NULL);
    g_autoptr (GUri) address = g_uri_parse (url, G_URI_FLAGS_NONE, NULL);
    if (domains) {
      for (guint i = 0; domains[i]; i++) append_domain (self, domains[i]);
    } else if (address && g_uri_get_host (address)) append_domain (self, g_uri_get_host (address));
  }
  g_object_bind_property (self->open_links, "active", self->domains, "sensitive", G_BINDING_SYNC_CREATE);
  g_object_bind_property (self->open_links, "active", self->domain_entry, "sensitive", G_BINDING_SYNC_CREATE);
  adw_preferences_group_add (links, self->open_links);
  adw_preferences_group_add (links, self->domain_entry);
  adw_preferences_group_add (links, self->domain_error);
  adw_preferences_group_add (links, self->domains);
  adw_preferences_page_add (page, links);
  adw_preferences_group_set_title (extensions, _("Extensions"));
  adw_preferences_group_set_description (extensions, _("Choose the extensions available to this app. Extension data is kept separate from regular browsing. Restart the app after changing extensions."));
  add_extension (self, extensions, "ublock", _("uBlock Origin"), NULL,
                 g_settings_get_boolean (EPHY_SETTINGS_WEB, EPHY_PREFS_WEB_ENABLE_ADBLOCK));
  add_extension (self, extensions, "keepassxc", _("KeePassXC"), NULL, TRUE);
  g_hash_table_add (ids, g_strdup ("ublock"));
  g_hash_table_add (ids, g_strdup ("keepassxc"));
  for (guint i = 0; inventory && i < json_array_get_length (inventory); i++) {
    JsonObject *item = json_array_get_object_element (inventory, i);
    const char *id = json_object_get_string_member_with_default (item, "id", "");
    const char *title = json_object_get_string_member_with_default (item, "name", "");
    const char *path = json_object_get_string_member_with_default (item, "path", "");
    if (!gnosis_web_app_extension_id_valid (id) || !*path || !strcmp (title, "uBlock Origin")) continue;
    add_extension (self, extensions, id, title, path, json_object_get_boolean_member_with_default (item, "enabled", FALSE));
    g_hash_table_add (ids, g_strdup (id));
  }
  saved = g_key_file_get_string_list (self->config, "Extensions", "enabled", NULL, NULL);
  for (guint i = 0; saved && saved[i]; i++) {
    g_autofree char *title = NULL;
    g_autofree char *path = NULL;
    if (g_hash_table_contains (ids, saved[i])) continue;
    title = g_key_file_get_string (self->config, saved[i], "name", NULL);
    path = g_key_file_get_string (self->config, saved[i], "path", NULL);
    add_extension (self, extensions, saved[i], title ? title : saved[i], path, TRUE);
  }
  adw_preferences_page_add (page, extensions);
  self->error = gtk_label_new (NULL);
  gtk_label_set_wrap (GTK_LABEL (self->error), TRUE);
  gtk_widget_add_css_class (self->error, "error");
  self->save = gtk_button_new_with_label (installing ? _("Continue") : _("Save"));
  gtk_widget_add_css_class (self->save, "suggested-action");
  gtk_widget_add_css_class (self->save, "pill");
  gtk_widget_set_halign (self->save, GTK_ALIGN_CENTER);
  g_signal_connect (self->save, "clicked", G_CALLBACK (accept_setup), self);
  adw_preferences_group_add (actions, self->error);
  adw_preferences_group_add (actions, self->save);
  adw_preferences_page_add (page, actions);
  adw_preferences_dialog_add (ADW_PREFERENCES_DIALOG (self), page);
  return GTK_WIDGET (self);
}
const char *ephy_web_app_dialog_get_name (EphyWebAppDialog *self) { return gtk_editable_get_text (GTK_EDITABLE (self->name)); }
const char *ephy_web_app_dialog_get_url (EphyWebAppDialog *self) { return gtk_editable_get_text (GTK_EDITABLE (self->url)); }
GIcon *ephy_web_app_dialog_get_icon (EphyWebAppDialog *self) { return self->icon; }
GKeyFile *ephy_web_app_dialog_dup_config (EphyWebAppDialog *self) {
  GKeyFile *config = g_key_file_ref (self->config);
  g_autoptr (GPtrArray) enabled = g_ptr_array_new ();
  for (guint i = 0; i < self->extensions->len; i++) {
    GtkWidget *row = g_ptr_array_index (self->extensions, i);
    const char *id = g_object_get_data (G_OBJECT (row), "extension-id");
    const char *path = g_object_get_data (G_OBJECT (row), "extension-path");
    if (path && *path) {
      g_key_file_set_string (config, id, "path", path);
      g_key_file_set_string (config, id, "name", adw_preferences_row_get_title (ADW_PREFERENCES_ROW (row)));
    }
    if (adw_switch_row_get_active (ADW_SWITCH_ROW (row))) g_ptr_array_add (enabled, (gpointer)id);
  }
  g_key_file_set_string_list (config, "Extensions", "enabled", (const char *const *)enabled->pdata, enabled->len);
  {
    g_autoptr (GPtrArray) domains = g_ptr_array_new_with_free_func (g_free);
    for (GtkWidget *chip = gtk_widget_get_first_child (self->domains); chip; chip = gtk_widget_get_next_sibling (chip))
      g_ptr_array_add (domains, g_strdup (g_object_get_data (G_OBJECT (chip), "domain")));
    g_key_file_set_boolean (config, "Links", "enabled", adw_switch_row_get_active (ADW_SWITCH_ROW (self->open_links)));
    g_key_file_set_string_list (config, "Links", "domains", (const char *const *)domains->pdata, domains->len);
  }
  return config;
}

gboolean ephy_web_app_dialog_apply (EphyWebAppDialog *self, EphyWebApplication *app, gboolean installing, GError **error) {
  g_autofree char *profile = ephy_web_application_get_profile_directory (app->id);
  g_autoptr (GKeyFile) config = ephy_web_app_dialog_dup_config (self);

  if (!ephy_web_application_set_extensions (app->id, config, error)) return FALSE;
  if (!installing) {
    g_autoptr (GBytes) png = ephy_web_app_dialog_dup_icon_bytes (self, error);
    g_autofree char *path = g_build_filename (profile, "app-icon.png", NULL);
    gsize length;
    const char *contents;
    if (!png) return FALSE;
    contents = g_bytes_get_data (png, &length);
    if (!g_file_set_contents_full (path, contents, length, G_FILE_SET_CONTENTS_CONSISTENT, 0600, error)) return FALSE;
    g_set_str (&app->name, ephy_web_app_dialog_get_name (self));
    g_set_str (&app->url, ephy_web_app_dialog_get_url (self));
    g_set_str (&app->icon_path, path);
    if (!ephy_web_application_save (app)) {
      g_set_error_literal (error, G_IO_ERROR, G_IO_ERROR_FAILED, _("Could not update the web app launcher.")); return FALSE;
    }
  }
  return TRUE;
}
