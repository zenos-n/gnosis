/* SPDX-License-Identifier: GPL-3.0-or-later */
#pragma once
#include <gio/gio.h>
G_BEGIN_DECLS
void gnosis_shared_start (GApplication *application);
void gnosis_shared_stop (void);
gboolean gnosis_shared_open_uri (const char *uri);
G_END_DECLS
