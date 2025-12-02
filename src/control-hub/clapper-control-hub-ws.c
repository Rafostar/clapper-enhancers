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

#include <clapper/clapper.h>

#include "clapper-control-hub.h"
#include "clapper-control-hub-actions.h"
#include "clapper-control-hub-json.h"
#include "clapper-control-hub-ws.h"

#define GST_CAT_DEFAULT clapper_control_hub_ws_debug
GST_DEBUG_CATEGORY_STATIC (GST_CAT_DEFAULT);

void
clapper_control_hub_ws_debug_init (void)
{
  GST_DEBUG_CATEGORY_INIT (GST_CAT_DEFAULT, "clappercontrolhubws",
      GST_DEBUG_FG_YELLOW, "Clapper Control Hub WebSocket");
}

static void
_ws_message_cb (SoupWebsocketConnection *connection, gint type, GBytes *message, ClapperControlHub *hub)
{
  ClapperPlayer *player;
  ClapperControlHubAction action;
  const gchar *text;

  if (G_UNLIKELY (type != SOUP_WEBSOCKET_DATA_TEXT)) {
    GST_WARNING_OBJECT (hub, "Received WS message with non-text data!");
    return;
  }

  text = g_bytes_get_data (message, NULL);

  if (G_UNLIKELY (text == NULL || *text == '\0')) {
    GST_WARNING_OBJECT (hub, "Received WS message without any text!");
    return;
  }

  action = clapper_control_hub_actions_get_action (text);

  if (action == CLAPPER_CONTROL_HUB_ACTION_INVALID) {
    GST_WARNING_OBJECT (hub, "Ignoring WS message with invalid action text");
    return;
  }

  player = clapper_reactable_get_player (CLAPPER_REACTABLE_CAST (hub));

  if (G_UNLIKELY (player == NULL))
    return;

  switch (action) {
    case CLAPPER_CONTROL_HUB_ACTION_TOGGLE_PLAY:
      switch (hub->state) {
        case CLAPPER_PLAYER_STATE_STOPPED:
        case CLAPPER_PLAYER_STATE_PAUSED:
          clapper_player_play (player);
          break;
        case CLAPPER_PLAYER_STATE_PLAYING:
          clapper_player_pause (player);
          break;
        default:
          break;
      }
      break;
    case CLAPPER_CONTROL_HUB_ACTION_PLAY:
      clapper_player_play (player);
      break;
    case CLAPPER_CONTROL_HUB_ACTION_PAUSE:
      clapper_player_pause (player);
      break;
    case CLAPPER_CONTROL_HUB_ACTION_STOP:
      clapper_player_stop (player);
      break;
    case CLAPPER_CONTROL_HUB_ACTION_SEEK:{
      gdouble position;
      if (clapper_control_hub_actions_parse_seek (text, &position))
        clapper_player_seek (player, position);
      break;
    }
    case CLAPPER_CONTROL_HUB_ACTION_SET_SPEED:{
      gdouble speed;
      if (clapper_control_hub_actions_parse_set_speed (text, &speed))
        clapper_player_set_speed (player, speed);
      break;
    }
    case CLAPPER_CONTROL_HUB_ACTION_SET_VOLUME:{
      gdouble volume;
      if (clapper_control_hub_actions_parse_set_volume (text, &volume))
        clapper_player_set_volume (player, volume);
      break;
    }
    case CLAPPER_CONTROL_HUB_ACTION_SET_MUTE:{
      gboolean mute;
      if (clapper_control_hub_actions_parse_set_mute (text, &mute))
        clapper_player_set_mute (player, mute);
      break;
    }
    case CLAPPER_CONTROL_HUB_ACTION_SET_PROGRESSION:{
      ClapperQueueProgressionMode mode;
      if (clapper_control_hub_actions_parse_set_progression (text, &mode))
        clapper_queue_set_progression_mode (clapper_player_get_queue (player), mode);
      break;
    }
    case CLAPPER_CONTROL_HUB_ACTION_ADD:{
      const gchar *uri;
      if (hub->queue_controllable && clapper_control_hub_actions_parse_add (text, &uri)) {
        ClapperMediaItem *item = clapper_media_item_new (uri);
        clapper_reactable_queue_append_sync (CLAPPER_REACTABLE_CAST (hub), item);
        gst_object_unref (item);
      }
      break;
    }
    case CLAPPER_CONTROL_HUB_ACTION_INSERT:{
      gchar *uri;
      guint after_id;
      if (hub->queue_controllable && clapper_control_hub_actions_parse_insert (text, &uri, &after_id)) {
        ClapperMediaItem *after_item = NULL;
        guint i;
        for (i = 0; i < hub->items->len; ++i) {
          ClapperMediaItem *tmp_after_item = (ClapperMediaItem *) g_ptr_array_index (hub->items, i);
          if (after_id == clapper_media_item_get_id (tmp_after_item)) {
            after_item = tmp_after_item;
            break;
          }
        }
        if (after_item) {
          ClapperMediaItem *item = clapper_media_item_new (uri);
          clapper_reactable_queue_insert_sync (CLAPPER_REACTABLE_CAST (hub), item, after_item);
          gst_object_unref (item);
        }
        g_free (uri);
      }
      break;
    }
    case CLAPPER_CONTROL_HUB_ACTION_SELECT:{
      guint id;
      if (hub->queue_controllable && clapper_control_hub_actions_parse_select (text, &id)) {
        guint i;
        for (i = 0; i < hub->items->len; ++i) {
          ClapperMediaItem *item = (ClapperMediaItem *) g_ptr_array_index (hub->items, i);
          if (id == clapper_media_item_get_id (item)) {
            clapper_queue_select_item (clapper_player_get_queue (player), item);
            break;
          }
        }
      }
      break;
    }
    case CLAPPER_CONTROL_HUB_ACTION_REMOVE:{
      guint id;
      if (hub->queue_controllable && clapper_control_hub_actions_parse_remove (text, &id)) {
        guint i;
        for (i = 0; i < hub->items->len; ++i) {
          ClapperMediaItem *item = (ClapperMediaItem *) g_ptr_array_index (hub->items, i);
          if (id == clapper_media_item_get_id (item)) {
            clapper_reactable_queue_remove_sync (CLAPPER_REACTABLE_CAST (hub), item);
            break;
          }
        }
      }
      break;
    }
    case CLAPPER_CONTROL_HUB_ACTION_CLEAR:
      if (hub->queue_controllable)
        clapper_reactable_queue_clear_sync (CLAPPER_REACTABLE_CAST (hub));
      break;
    default:
      g_assert_not_reached ();
      break;
  }

  gst_object_unref (player);
}

static void
_ws_connection_closed_cb (SoupWebsocketConnection *connection, ClapperControlHub *hub)
{
  GST_INFO_OBJECT (hub, "WebSocket connection closed: %p", connection);
  g_ptr_array_remove (hub->ws_connections, connection);
}

void
clapper_control_hub_ws_connection_cb (SoupServer *server, SoupServerMessage *msg,
    const gchar *path, SoupWebsocketConnection *connection, ClapperControlHub *hub)
{
  GST_INFO_OBJECT (hub, "New WebSocket connection: %p", connection);

  g_signal_connect (connection, "message", G_CALLBACK (_ws_message_cb), hub);
  g_signal_connect (connection, "closed", G_CALLBACK (_ws_connection_closed_cb), hub);
  g_ptr_array_add (hub->ws_connections, g_object_ref (connection));

  if (G_LIKELY (soup_websocket_connection_get_state (connection) == SOUP_WEBSOCKET_STATE_OPEN)) {
    gchar *init_data = clapper_control_hub_json_build_default (hub, TRUE);
    soup_websocket_connection_send_text (connection, init_data);
    g_free (init_data);
  }
}

void
clapper_control_hub_ws_send (ClapperControlHub *hub, const gchar *text)
{
  guint i;

  GST_LOG_OBJECT (hub, "Sending WS message to clients: \"%s\"", text);

  for (i = 0; i < hub->ws_connections->len; ++i) {
    SoupWebsocketConnection *connection = g_ptr_array_index (hub->ws_connections, i);

    if (soup_websocket_connection_get_state (connection) != SOUP_WEBSOCKET_STATE_OPEN)
      continue;

    soup_websocket_connection_send_text (connection, text);
  }
}
