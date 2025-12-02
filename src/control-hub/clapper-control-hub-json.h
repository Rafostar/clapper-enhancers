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
#include <clapper/clapper.h>

#include "clapper-control-hub.h"

G_BEGIN_DECLS

#define __JSON_EMPTY_EVENT(n) "{\"event\":\"" n "\"}"
#define __JSON_CHANGED_EVENT(n,t) "{\"event\":\"" n "_changed\",\"" n "\":" t "}"
#define __JSON_CUSTOM_EVENT(n,k1,t1,k2,t2) "{\"event\":\"" n "\",\"" k1 "\":" t1 ",\"" k2 "\":" t2 "}"

#define clapper_control_hub_json_fill_state_changed_message(string,state) \
  g_snprintf (string, sizeof (string), __JSON_CHANGED_EVENT ("state", "%u"), (state))

#define clapper_control_hub_json_fill_position_changed_message(string,position) \
  g_snprintf (string, sizeof (string), __JSON_CHANGED_EVENT ("position", "%.3lf"), (position))

#define clapper_control_hub_json_fill_speed_changed_message(string,speed) \
  g_snprintf (string, sizeof (string), __JSON_CHANGED_EVENT ("speed", "%.2lf"), (speed))

#define clapper_control_hub_json_fill_volume_changed_message(string,volume) \
  g_snprintf (string, sizeof (string), __JSON_CHANGED_EVENT ("volume", "%.2lf"), (volume))

#define clapper_control_hub_json_fill_mute_changed_message(string,mute) \
  g_snprintf (string, sizeof (string), __JSON_CHANGED_EVENT ("mute", "%s"), ((mute) ? "true" : "false"))

#define clapper_control_hub_json_fill_played_index_changed_message(string,index) \
  g_snprintf (string, sizeof (string), __JSON_CHANGED_EVENT ("played_index", "%u"), (index))

#define clapper_control_hub_json_fill_progression_changed_message(string,mode) \
  g_snprintf (string, sizeof (string), __JSON_CHANGED_EVENT ("progression", "%u"), (mode))

#define clapper_control_hub_json_fill_item_updated_message(string,item_id,flags) \
  g_snprintf (string, sizeof (string), __JSON_CUSTOM_EVENT ("item_updated", "id", "%u", "flags", "%u"), (item_id), (flags))

#define clapper_control_hub_json_fill_item_added_message(string,item_id,index) \
  g_snprintf (string, sizeof (string), __JSON_CUSTOM_EVENT ("item_added", "id", "%u", "index", "%u"), (item_id), (index))

#define clapper_control_hub_json_fill_item_removed_message(string,item_id,index) \
  g_snprintf (string, sizeof (string), __JSON_CUSTOM_EVENT ("item_removed", "id", "%u", "index", "%u"), (item_id), (index))

#define clapper_control_hub_json_fill_item_repositioned_message(string,before,after) \
  g_snprintf (string, sizeof (string), __JSON_CUSTOM_EVENT ("item_repositioned", "before", "%u", "after", "%u"), (before), (after))

#define clapper_control_hub_json_fill_queue_cleared_message(string) \
  g_snprintf (string, sizeof (string), __JSON_EMPTY_EVENT ("queue_cleared"))

G_GNUC_INTERNAL
gchar * clapper_control_hub_json_build_default (ClapperControlHub *hub, gboolean as_event);

G_GNUC_INTERNAL
gchar * clapper_control_hub_json_build_item_info (ClapperControlHub *hub, ClapperMediaItem *item, gboolean with_timeline);

G_END_DECLS
