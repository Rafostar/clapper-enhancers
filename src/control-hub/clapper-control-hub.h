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
#include <glib-object.h>
#include <gst/gst.h>
#include <clapper/clapper.h>
#include <libsoup/soup.h>

#include "clapper-control-hub-mdns.h"

G_BEGIN_DECLS

#define CLAPPER_TYPE_CONTROL_HUB (clapper_control_hub_get_type())
#define CLAPPER_CONTROL_HUB_CAST(obj) ((ClapperControlHub *)(obj))

G_GNUC_INTERNAL
G_DECLARE_FINAL_TYPE (ClapperControlHub, clapper_control_hub, CLAPPER, CONTROL_HUB, GstObject)

struct _ClapperControlHub
{
  GstObject parent;

  gboolean running;

  SoupServer *server;
  GPtrArray *ws_connections;
  ClapperControlHubMdns *mdns;

  GPtrArray *items;
  ClapperMediaItem *played_item;
  guint played_index;

  ClapperPlayerState state;
  gdouble position;
  gdouble speed;
  gdouble volume;
  gboolean mute;
  ClapperQueueProgressionMode progression;

  gboolean active;
  gboolean queue_controllable;
};

G_END_DECLS
