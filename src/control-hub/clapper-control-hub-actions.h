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

G_BEGIN_DECLS

typedef enum
{
  CLAPPER_CONTROL_HUB_ACTION_INVALID = 0,
  CLAPPER_CONTROL_HUB_ACTION_TOGGLE_PLAY,
  CLAPPER_CONTROL_HUB_ACTION_PLAY,
  CLAPPER_CONTROL_HUB_ACTION_PAUSE,
  CLAPPER_CONTROL_HUB_ACTION_STOP,
  CLAPPER_CONTROL_HUB_ACTION_SEEK,
  CLAPPER_CONTROL_HUB_ACTION_SET_SPEED,
  CLAPPER_CONTROL_HUB_ACTION_SET_VOLUME,
  CLAPPER_CONTROL_HUB_ACTION_SET_MUTE,
  CLAPPER_CONTROL_HUB_ACTION_SET_PROGRESSION,
  CLAPPER_CONTROL_HUB_ACTION_ADD,
  CLAPPER_CONTROL_HUB_ACTION_INSERT,
  CLAPPER_CONTROL_HUB_ACTION_SELECT,
  CLAPPER_CONTROL_HUB_ACTION_REMOVE,
  CLAPPER_CONTROL_HUB_ACTION_CLEAR
} ClapperControlHubAction;

G_GNUC_INTERNAL
ClapperControlHubAction clapper_control_hub_actions_get_action (const gchar *text);

G_GNUC_INTERNAL
gboolean clapper_control_hub_actions_parse_seek (const gchar *text, gdouble *position);

G_GNUC_INTERNAL
gboolean clapper_control_hub_actions_parse_set_speed (const gchar *text, gdouble *speed);

G_GNUC_INTERNAL
gboolean clapper_control_hub_actions_parse_set_volume (const gchar *text, gdouble *volume);

G_GNUC_INTERNAL
gboolean clapper_control_hub_actions_parse_set_mute (const gchar *text, gboolean *mute);

G_GNUC_INTERNAL
gboolean clapper_control_hub_actions_parse_set_progression (const gchar *text, ClapperQueueProgressionMode *mode);

G_GNUC_INTERNAL
gboolean clapper_control_hub_actions_parse_add (const gchar *text, const gchar **uri);

G_GNUC_INTERNAL
gboolean clapper_control_hub_actions_parse_insert (const gchar *text, gchar **uri, guint *after_id);

G_GNUC_INTERNAL
gboolean clapper_control_hub_actions_parse_select (const gchar *text, guint *id);

G_GNUC_INTERNAL
gboolean clapper_control_hub_actions_parse_remove (const gchar *text, guint *id);

G_END_DECLS
