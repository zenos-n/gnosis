/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "config.h"
#include "prefs-web-apps-page.h"
#include "ephy-web-app-dialog.h"
#include "ephy-web-app-utils.h"
#include "web-app-config.h"
#include <glib/gi18n.h>

struct _PrefsWebAppsPage {
  AdwPreferencesPage parent_instance;
  GtkWidget *list, *empty;
};
G_DEFINE_FINAL_TYPE (PrefsWebAppsPage, prefs_web_apps_page, ADW_TYPE_PREFERENCES_PAGE)
static void refresh (PrefsWebAppsPage *self);
static void show_error (PrefsWebAppsPage *self, const char *message) {
  AdwDialog *dialog = adw_alert_dialog_new (_("Could Not Update Web App"), message);
  adw_alert_dialog_set_body_use_markup (ADW_ALERT_DIALOG (dialog), FALSE);
  adw_alert_dialog_add_response (ADW_ALERT_DIALOG (dialog), "close", _("Close"));
  adw_dialog_present (dialog, GTK_WIDGET (self));
}
static void launch_app (GtkButton *button, PrefsWebAppsPage *self) {
  const char *id = g_object_get_data (G_OBJECT (button), "app-id");
  ephy_web_application_launch (id);
}
static void save_app (EphyWebAppDialog *dialog, PrefsWebAppsPage *self) {
  EphyWebApplication *app = g_object_get_data (G_OBJECT (dialog), "web-app");
  g_autoptr (GError) error = NULL;
  if (!ephy_web_app_dialog_apply (dialog, app, FALSE, &error)) { show_error (self, error->message); return; }
  refresh (self);
  adw_dialog_close (ADW_DIALOG (dialog));
}
static void edit_app (AdwActionRow *row, PrefsWebAppsPage *self) {
  const char *id = g_object_get_data (G_OBJECT (row), "app-id");
  g_autofree char *profile = ephy_web_application_get_profile_directory (id);
  g_autoptr (EphyWebApplication) app = ephy_web_application_for_profile_directory (profile, EPHY_WEB_APP_NO_TMP_ICON);
  g_autoptr (GError) error = NULL;
  g_autoptr (GKeyFile) config = gnosis_web_app_config_load (profile, &error);
  g_autoptr (GIcon) icon = NULL;
  GtkWidget *dialog;
  if (!app || !config) { show_error (self, error ? error->message : _("The web app is no longer installed.")); return; }
  if (app->icon_path && g_path_is_absolute (app->icon_path)) {
    g_autoptr (GFile) file = g_file_new_for_path (app->icon_path);
    icon = g_file_icon_new (file);
  } else icon = g_themed_icon_new (app->icon_path ? app->icon_path : "application-x-executable");
  dialog = ephy_web_app_dialog_new (app->name, app->url, icon, config, FALSE);
  g_object_set_data_full (G_OBJECT (dialog), "web-app", g_steal_pointer (&app), (GDestroyNotify)ephy_web_application_free);
  g_signal_connect_object (dialog, "accepted", G_CALLBACK (save_app), self, 0);
  adw_dialog_present (ADW_DIALOG (dialog), GTK_WIDGET (self));
}
static void confirm_remove (AdwAlertDialog *dialog, const char *response, PrefsWebAppsPage *self) {
  const char *id = g_object_get_data (G_OBJECT (dialog), "app-id");
  if (strcmp (response, "remove")) return;
  if (!ephy_web_application_delete (id, NULL)) show_error (self, _("The web app could not be removed."));
  refresh (self);
}
static void remove_app (GtkButton *button, PrefsWebAppsPage *self) {
  const char *id = g_object_get_data (G_OBJECT (button), "app-id");
  AdwDialog *dialog = adw_alert_dialog_new (_("Remove Web App?"), _("The app and its browsing data will be removed."));
  g_object_set_data_full (G_OBJECT (dialog), "app-id", g_strdup (id), g_free);
  adw_alert_dialog_add_responses (ADW_ALERT_DIALOG (dialog), "cancel", _("Cancel"), "remove", _("Remove"), NULL);
  adw_alert_dialog_set_response_appearance (ADW_ALERT_DIALOG (dialog), "remove", ADW_RESPONSE_DESTRUCTIVE);
  adw_alert_dialog_set_default_response (ADW_ALERT_DIALOG (dialog), "cancel");
  adw_alert_dialog_set_close_response (ADW_ALERT_DIALOG (dialog), "cancel");
  g_signal_connect_object (dialog, "response", G_CALLBACK (confirm_remove), self, 0);
  adw_dialog_present (dialog, GTK_WIDGET (self));
}
static void refresh (PrefsWebAppsPage *self) {
  GList *apps = ephy_web_application_get_application_list ();
  gtk_list_box_remove_all (GTK_LIST_BOX (self->list));
  gtk_widget_set_visible (self->list, apps != NULL);
  gtk_widget_set_visible (self->empty, apps == NULL);
  for (GList *item = apps; item; item = item->next) {
    EphyWebApplication *app = item->data;
    GtkWidget *row = adw_action_row_new ();
    GtkWidget *image = gtk_image_new ();
    GtkWidget *launch = gtk_button_new_from_icon_name ("media-playback-start-symbolic");
    GtkWidget *remove = app->declarative ? NULL : gtk_button_new_from_icon_name ("user-trash-symbolic");
    adw_preferences_row_set_title (ADW_PREFERENCES_ROW (row), app->name);
    adw_preferences_row_set_use_markup (ADW_PREFERENCES_ROW (row), FALSE);
    adw_action_row_set_subtitle (ADW_ACTION_ROW (row), app->url);
    gtk_list_box_row_set_activatable (GTK_LIST_BOX_ROW (row), TRUE);
    if (app->icon_path && g_path_is_absolute (app->icon_path)) gtk_image_set_from_file (GTK_IMAGE (image), app->icon_path);
    else gtk_image_set_from_icon_name (GTK_IMAGE (image), app->icon_path ? app->icon_path : "application-x-executable");
    gtk_image_set_pixel_size (GTK_IMAGE (image), 40);
    adw_action_row_add_prefix (ADW_ACTION_ROW (row), image);
    gtk_widget_set_valign (launch, GTK_ALIGN_CENTER);
    if (remove) gtk_widget_set_valign (remove, GTK_ALIGN_CENTER);
    gtk_widget_set_tooltip_text (launch, _("Launch Web App"));
    if (remove) gtk_widget_set_tooltip_text (remove, _("Remove Web App"));
    gtk_widget_add_css_class (launch, "flat");
    if (remove) gtk_widget_add_css_class (remove, "flat");
    adw_action_row_add_suffix (ADW_ACTION_ROW (row), launch);
    if (remove)
      adw_action_row_add_suffix (ADW_ACTION_ROW (row), remove);
    g_object_set_data_full (G_OBJECT (row), "app-id", g_strdup (app->id), g_free);
    g_object_set_data_full (G_OBJECT (launch), "app-id", g_strdup (app->id), g_free);
    if (remove) g_object_set_data_full (G_OBJECT (remove), "app-id", g_strdup (app->id), g_free);
    if (!app->declarative)
      g_signal_connect (row, "activated", G_CALLBACK (edit_app), self);
    else
      gtk_list_box_row_set_activatable (GTK_LIST_BOX_ROW (row), FALSE);
    g_signal_connect (launch, "clicked", G_CALLBACK (launch_app), self);
    if (remove) g_signal_connect (remove, "clicked", G_CALLBACK (remove_app), self);
    gtk_list_box_append (GTK_LIST_BOX (self->list), row);
  }
  ephy_web_application_free_application_list (apps);
}
static void mapped (GtkWidget *widget) {
  GTK_WIDGET_CLASS (prefs_web_apps_page_parent_class)->map (widget);
  refresh (EPHY_PREFS_WEB_APPS_PAGE (widget));
}
static void prefs_web_apps_page_class_init (PrefsWebAppsPageClass *klass) {
  GTK_WIDGET_CLASS (klass)->map = mapped;
}
static void prefs_web_apps_page_init (PrefsWebAppsPage *self) {
  AdwPreferencesGroup *group = ADW_PREFERENCES_GROUP (adw_preferences_group_new ());
  adw_preferences_page_set_name (ADW_PREFERENCES_PAGE (self), "web-apps-page");
  adw_preferences_page_set_title (ADW_PREFERENCES_PAGE (self), _("Web Apps"));
  adw_preferences_page_set_icon_name (ADW_PREFERENCES_PAGE (self), "application-x-executable-symbolic");
  adw_preferences_group_set_description (group, _("Select an app to change its name, icon, address, link associations, or extensions."));
  self->list = gtk_list_box_new ();
  gtk_list_box_set_selection_mode (GTK_LIST_BOX (self->list), GTK_SELECTION_NONE);
  gtk_widget_add_css_class (self->list, "boxed-list");
  self->empty = adw_status_page_new ();
  adw_status_page_set_icon_name (ADW_STATUS_PAGE (self->empty), "application-x-executable-symbolic");
  adw_status_page_set_title (ADW_STATUS_PAGE (self->empty), _("No Web Apps"));
  adw_status_page_set_description (ADW_STATUS_PAGE (self->empty), _("Open a website and choose Install as Web App from the menu."));
  adw_preferences_group_add (group, self->list);
  adw_preferences_group_add (group, self->empty);
  adw_preferences_page_add (ADW_PREFERENCES_PAGE (self), group);
}
