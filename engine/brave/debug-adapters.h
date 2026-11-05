/* SPDX-License-Identifier: GPL-3.0-or-later */
#pragma once
#include <glib.h>

typedef struct {
  const char *id, *group, *title, *description;
} GnosisBraveAdapter;

/* IDs are shared by preferences, the page bridge and the pinned native shim. */
static const GnosisBraveAdapter gnosis_brave_adapters[] = {
  {"keepassxc", "Extensions", "KeePassXC", "Password, one-time-code and passkey integration, including field observers."},
  {"ublock", "Extensions", "uBlock Origin", "The bundled full extension and its content scripts."},
  {"browser-preferences", "Browser Services", "Browser Preferences", "Font, storage, popup, cookie-banner and content-blocking preference bridge."},
  {"downloads", "Browser Services", "Native Downloads", "GTK download progress and file ownership instead of Chromium downloads."},
  {"find", "Browser Services", "Native Find Bridge", "Search the page through Chromium's native find service."},
  {"extension-events", "Browser Services", "Extension and Zoom Events", "Forward extension inventory and page zoom changes to the shell."},
  {"network-events", "Browser Services", "Network Inspection", "Network response events, resource metadata and favicon resource access."},
  {"frame-sessions", "Page Adapters", "Cross-Origin Frame Sessions", "Attach native adapters to out-of-process frames."},
  {"page-style", "Page Adapters", "Page Styling", "Selection colors, native overlay colors, and dropdown defaults. DOM/CSS inspection is released after each batch of style updates."},
  {"scrollbars", "Page Adapters", "Native Scrollbar Theme", "The compiled libadwaita theme, including thin scrollbars. Disable to use upstream Brave scrollbars."},
  {"context-menu", "Page Adapters", "Context Menus", "Injected hit testing and GTK page context menus."},
  {"document-metadata", "Page Adapters", "Document Metadata", "Title, icon and document-activity notifications. Vim requires this."},
  {"vim", "Page Adapters", "Vim Controller", "Inject the Vim navigation and field editing controllers when Vim mode is enabled."},
  {"user-content", "Page Adapters", "Custom Styles and Scripts", "Apply the custom stylesheet and JavaScript from Appearance."},
  {"popup-sizing", "Page Adapters", "Extension Popup Sizing", "Observe extension popup documents and adjust their GTK container."},
  {"permission-dialogs", "Native Dialogs", "Permission Prompts", "GTK replacements for website permission prompts."},
  {"extension-dialogs", "Native Dialogs", "Extension Install and Remove Prompts", "GTK replacements for extension confirmation dialogs."},
  {"device-choosers", "Native Dialogs", "Device Choosers", "GTK Bluetooth, USB, serial and HID device selection."},
  {"external-apps", "Native Dialogs", "External Applications", "GTK confirmation before opening website links in another application."},
  {"file-access", "Native Dialogs", "File Access Prompts", "GTK permission prompts for File System Access."},
  {"screen-capture", "Native Dialogs", "Screen Sharing", "The screen-sharing portal and GTK confirmation bridge."},
  {"passkeys", "Native Dialogs", "Passkeys and Security Keys", "GTK presentation of Chromium authenticator dialogs. Separate from KeePassXC."},
  {"script-dialogs", "Native Dialogs", "Website Dialogs", "GTK alert, confirm and prompt dialogs. Close safety checks remain enabled."},
  {"translation", "Native Dialogs", "Page Translation", "GTK controls for Chromium's full-page translation service."},
  {"zoom-osd", "Native Dialogs", "Zoom OSD", "Native zoom feedback instead of Chromium's zoom bubble."},
  {"notifications", "Native Dialogs", "Notification Identity", "Present browser notifications with Gnosis's name and icon."},
  {"infobars", "Native Dialogs", "Hide Chromium Banners", "Suppress Chromium's infobar container, including command-line warnings."},
};
static inline gboolean gnosis_brave_adapter_known(const char *id) {
  for (guint i = 0; i < G_N_ELEMENTS(gnosis_brave_adapters); ++i)
    if (g_str_equal(id, gnosis_brave_adapters[i].id)) return TRUE;
  return FALSE;
}
