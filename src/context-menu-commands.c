/* -*- Mode: C; tab-width: 2; indent-tabs-mode: nil; c-basic-offset: 2 -*- */
/*
 *  Copyright © 2000-2003 Marco Pesenti Gritti
 *  Copyright © 2017 Igalia S.L.
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

#include "config.h"

#include "context-menu-commands.h"

#include "ephy-embed-container.h"
#include "ephy-embed-utils.h"
#include "ephy-file-helpers.h"
#include "ephy-prefs.h"
#include "ephy-settings.h"
#include "ephy-shell.h"
#include "ephy-web-view.h"

void
context_cmd_search_selection (GSimpleAction *action,
                              GVariant      *parameter,
                              gpointer       user_data)
{
  EphyEmbed *embed, *new_embed;
  const char *search_term;
  char *search_url;

  embed = ephy_embed_container_get_active_child (EPHY_EMBED_CONTAINER (user_data));
  g_assert (EPHY_IS_EMBED (embed));

  search_term = g_variant_get_string (parameter, NULL);
  search_url = ephy_embed_utils_autosearch_address (search_term);
  new_embed = ephy_shell_new_tab (ephy_shell_get_default (),
                                  EPHY_WINDOW (user_data), embed, EPHY_NEW_TAB_APPEND_AFTER | EPHY_NEW_TAB_JUMP);
  ephy_web_view_load_url (ephy_embed_get_web_view (new_embed), search_url);

  gtk_widget_grab_focus (GTK_WIDGET (ephy_embed_get_web_view (new_embed)));

  g_free (search_url);
}

void
context_cmd_open_selection (GSimpleAction *action,
                            GVariant      *parameter,
                            gpointer       user_data)
{
  EphyEmbed *embed;
  const char *open_term;

  embed = ephy_embed_container_get_active_child (EPHY_EMBED_CONTAINER (user_data));
  g_assert (EPHY_IS_EMBED (embed));

  open_term = g_variant_get_string (parameter, NULL);
  ephy_web_view_load_url (ephy_embed_get_web_view (embed), open_term);
}

void
context_cmd_open_selection_in_new_tab (GSimpleAction *action,
                                       GVariant      *parameter,
                                       gpointer       user_data)
{
  EphyEmbed *embed, *new_embed;
  EphyWebView *web_view;
  const char *open_term;
  EphyNewTabFlags flags = EPHY_NEW_TAB_APPEND_AFTER;

  embed = ephy_embed_container_get_active_child (EPHY_EMBED_CONTAINER (user_data));
  g_assert (EPHY_IS_EMBED (embed));

  open_term = g_variant_get_string (parameter, NULL);
  if (ephy_shell_open_external_uri (ephy_shell_get_default (), open_term, FALSE))
    return;
  if (g_settings_get_boolean (EPHY_SETTINGS_WEB, EPHY_PREFS_WEB_SWITCH_TO_NEW_TAB))
    flags |= EPHY_NEW_TAB_JUMP;

  new_embed = ephy_shell_new_tab (ephy_shell_get_default (),
                                  EPHY_WINDOW (user_data), embed, flags);
  web_view = ephy_embed_get_web_view (new_embed);
  ephy_web_view_load_url (web_view, open_term);

  if (g_settings_get_boolean (EPHY_SETTINGS_WEB, EPHY_PREFS_WEB_SWITCH_TO_NEW_TAB))
    gtk_widget_grab_focus (GTK_WIDGET (web_view));
}

void
context_cmd_open_selection_in_new_window (GSimpleAction *action,
                                          GVariant      *parameter,
                                          gpointer       user_data)
{
  EphyEmbed *embed, *new_embed;
  const char *open_term;

  embed = ephy_embed_container_get_active_child (EPHY_EMBED_CONTAINER (user_data));
  g_assert (EPHY_IS_EMBED (embed));

  open_term = g_variant_get_string (parameter, NULL);
  if (ephy_shell_open_external_uri (ephy_shell_get_default (), open_term, TRUE))
    return;
  new_embed = ephy_shell_new_tab (ephy_shell_get_default (),
                                  ephy_window_new (), embed, 0);
  ephy_web_view_load_url (ephy_embed_get_web_view (new_embed), open_term);
}

void
context_cmd_open_selection_in_incognito_window (GSimpleAction *action,
                                                GVariant      *parameter,
                                                gpointer       user_data)
{
  const char *open_term;

  open_term = g_variant_get_string (parameter, NULL);
  ephy_open_incognito_window (open_term);
}
