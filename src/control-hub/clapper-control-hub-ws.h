/* Clapper Enhancer Control Hub
 * Copyright (C) 2025 Rafał Dzięgiel <rafostar.github@gmail.com>
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, see
 * <https://www.gnu.org/licenses/>.
 */

#pragma once

#include <glib.h>
#include <libsoup/soup.h>

#include "clapper-control-hub.h"

G_BEGIN_DECLS

void clapper_control_hub_ws_debug_init (void);

void clapper_control_hub_ws_connection_cb (SoupServer *server, SoupServerMessage *msg, const gchar *path, SoupWebsocketConnection *connection, ClapperControlHub *hub);

void clapper_control_hub_ws_send (ClapperControlHub *hub, const gchar *text);

G_END_DECLS
