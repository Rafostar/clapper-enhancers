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

/**
 * ClapperControlHub:
 *
 * Allows to control playback remotely.
 */

#include <gio/gio.h>
#include <gmodule.h>
#include <libpeas.h>

#include "clapper-control-hub.h"
#include "clapper-control-hub-json.h"
#include "clapper-control-hub-ws.h"

#define DEFAULT_ACTIVE FALSE
#define DEFAULT_QUEUE_CONTROLLABLE FALSE

#define WS_EVENT_SIZE 128

#define GST_CAT_DEFAULT clapper_control_hub_debug
GST_DEBUG_CATEGORY_STATIC (GST_CAT_DEFAULT);

G_MODULE_EXPORT void peas_register_types (PeasObjectModule *module);

enum
{
  PROP_0,
  PROP_ACTIVE,
  PROP_QUEUE_CONTROLLABLE,
  PROP_LAST
};

static GParamSpec *param_specs[PROP_LAST] = { NULL, };

static void
_clear_stored_queue (ClapperControlHub *self)
{
  if (self->items->len > 0)
    g_ptr_array_remove_range (self->items, 0, self->items->len);

  gst_clear_object (&self->played_item);
  self->played_index = CLAPPER_QUEUE_INVALID_POSITION;
}

static void
clapper_control_hub_state_changed (ClapperReactable *reactable, ClapperPlayerState state)
{
  ClapperControlHub *self = CLAPPER_CONTROL_HUB_CAST (reactable);

  GST_DEBUG_OBJECT (self, "Playback state changed to: %u", state);
  self->state = state;

  if (self->running && self->ws_connections->len > 0) {
    gchar data[WS_EVENT_SIZE];
    clapper_control_hub_json_fill_state_changed_message (data, self->state);
    clapper_control_hub_ws_send (self, data);
  }
}

static void
clapper_control_hub_position_changed (ClapperReactable *reactable, gdouble position)
{
  ClapperControlHub *self = CLAPPER_CONTROL_HUB_CAST (reactable);

  /* Limit to seconds */
  if ((gint) self->position == (gint) position)
    return;

  GST_LOG_OBJECT (self, "Position changed to: %.3lf", position);
  self->position = position;

  if (self->running && self->ws_connections->len > 0) {
    gchar data[WS_EVENT_SIZE];
    clapper_control_hub_json_fill_position_changed_message (data, self->position);
    clapper_control_hub_ws_send (self, data);
  }
}

static void
clapper_control_hub_speed_changed (ClapperReactable *reactable, gdouble speed)
{
  ClapperControlHub *self = CLAPPER_CONTROL_HUB_CAST (reactable);

  GST_LOG_OBJECT (self, "Speed changed to: %.2lf", speed);
  self->speed = speed;

  if (self->running && self->ws_connections->len > 0) {
    gchar data[WS_EVENT_SIZE];
    clapper_control_hub_json_fill_speed_changed_message (data, self->speed);
    clapper_control_hub_ws_send (self, data);
  }
}

static void
clapper_control_hub_volume_changed (ClapperReactable *reactable, gdouble volume)
{
  ClapperControlHub *self = CLAPPER_CONTROL_HUB_CAST (reactable);

  GST_LOG_OBJECT (self, "Volume changed to: %.2lf", volume);
  self->volume = volume;

  if (self->running && self->ws_connections->len > 0) {
    gchar data[WS_EVENT_SIZE];
    clapper_control_hub_json_fill_volume_changed_message (data, self->volume);
    clapper_control_hub_ws_send (self, data);
  }
}

static void
clapper_control_hub_mute_changed (ClapperReactable *reactable, gboolean mute)
{
  ClapperControlHub *self = CLAPPER_CONTROL_HUB_CAST (reactable);

  GST_LOG_OBJECT (self, "Mute changed to: %s", (mute) ? "enabled" : "disabled");
  self->mute = mute;

  if (self->running && self->ws_connections->len > 0) {
    gchar data[WS_EVENT_SIZE];
    clapper_control_hub_json_fill_mute_changed_message (data, self->mute);
    clapper_control_hub_ws_send (self, data);
  }
}

static void
clapper_control_hub_played_item_changed (ClapperReactable *reactable, ClapperMediaItem *item)
{
  ClapperControlHub *self = CLAPPER_CONTROL_HUB_CAST (reactable);

  GST_DEBUG_OBJECT (self, "Played item changed to: %" GST_PTR_FORMAT, item);

  gst_object_replace ((GstObject **) &self->played_item, GST_OBJECT_CAST (item));
  if (!g_ptr_array_find (self->items, self->played_item, &self->played_index))
    self->played_index = CLAPPER_QUEUE_INVALID_POSITION;

  if (self->running && self->ws_connections->len > 0) {
    gchar data[WS_EVENT_SIZE];
    clapper_control_hub_json_fill_played_index_changed_message (data, self->played_index);
    clapper_control_hub_ws_send (self, data);
  }
}

static void
clapper_control_hub_item_updated (ClapperReactable *reactable, ClapperMediaItem *item, ClapperReactableItemUpdatedFlags flags)
{
  ClapperControlHub *self = CLAPPER_CONTROL_HUB_CAST (reactable);

  /* Ignore updates with flags this enhancer does not care about */
  flags &= ~(CLAPPER_REACTABLE_ITEM_UPDATED_REDIRECT_URI | CLAPPER_REACTABLE_ITEM_UPDATED_CACHE_LOCATION);
  if (flags == 0)
    return;

  if (self->running && self->ws_connections->len > 0) {
    gchar data[WS_EVENT_SIZE];
    clapper_control_hub_json_fill_item_updated_message (data, clapper_media_item_get_id (item), flags);
    clapper_control_hub_ws_send (self, data);
  }
}

static void
clapper_control_hub_queue_item_added (ClapperReactable *reactable, ClapperMediaItem *item, guint index)
{
  ClapperControlHub *self = CLAPPER_CONTROL_HUB_CAST (reactable);

  GST_DEBUG_OBJECT (self, "Queue %" GST_PTR_FORMAT " added, position: %u", item, index);
  g_ptr_array_insert (self->items, index, gst_object_ref (item));

  if (self->running && self->ws_connections->len > 0) {
    gchar data[WS_EVENT_SIZE];
    clapper_control_hub_json_fill_item_added_message (data, clapper_media_item_get_id (item), index);
    clapper_control_hub_ws_send (self, data);
  }
}

static void
clapper_control_hub_queue_item_removed (ClapperReactable *reactable, ClapperMediaItem *item, guint index)
{
  ClapperControlHub *self = CLAPPER_CONTROL_HUB_CAST (reactable);

  GST_DEBUG_OBJECT (self, "Queue %" GST_PTR_FORMAT " removed, position: %u", item, index);

  if (item == self->played_item) {
    gst_clear_object (&self->played_item);
    self->played_index = CLAPPER_QUEUE_INVALID_POSITION;
  }
  g_ptr_array_remove_index (self->items, index);

  if (self->running && self->ws_connections->len > 0) {
    gchar data[WS_EVENT_SIZE];
    clapper_control_hub_json_fill_item_removed_message (data, clapper_media_item_get_id (item), index);
    clapper_control_hub_ws_send (self, data);
  }
}

static void
clapper_control_hub_queue_item_repositioned (ClapperReactable *reactable, guint before, guint after)
{
  ClapperControlHub *self = CLAPPER_CONTROL_HUB_CAST (reactable);
  ClapperMediaItem *item;

  GST_DEBUG_OBJECT (self, "Queue item repositioned: %u -> %u", before, after);

  item = (ClapperMediaItem *) g_ptr_array_steal_index (self->items, before);
  g_ptr_array_insert (self->items, after, item);

  if (self->running && self->ws_connections->len > 0) {
    gchar data[WS_EVENT_SIZE];
    clapper_control_hub_json_fill_item_repositioned_message (data, before, after);
    clapper_control_hub_ws_send (self, data);
  }
}

static void
clapper_control_hub_queue_cleared (ClapperReactable *reactable)
{
  ClapperControlHub *self = CLAPPER_CONTROL_HUB_CAST (reactable);

  GST_DEBUG_OBJECT (self, "Queue cleared");
  _clear_stored_queue (self);

  if (self->running && self->ws_connections->len > 0) {
    gchar data[WS_EVENT_SIZE];
    clapper_control_hub_json_fill_queue_cleared_message (data);
    clapper_control_hub_ws_send (self, data);
  }
}

static void
clapper_control_hub_queue_progression_changed (ClapperReactable *reactable, ClapperQueueProgressionMode mode)
{
  ClapperControlHub *self = CLAPPER_CONTROL_HUB_CAST (reactable);

  GST_DEBUG_OBJECT (self, "Queue progression changed to: %u", mode);
  self->progression = mode;

  if (self->running && self->ws_connections->len > 0) {
    gchar data[WS_EVENT_SIZE];
    clapper_control_hub_json_fill_progression_changed_message (data, self->progression);
    clapper_control_hub_ws_send (self, data);
  }
}

static void
clapper_control_hub_reactable_iface_init (ClapperReactableInterface *iface)
{
  iface->state_changed = clapper_control_hub_state_changed;
  iface->position_changed = clapper_control_hub_position_changed;
  iface->speed_changed = clapper_control_hub_speed_changed;
  iface->volume_changed = clapper_control_hub_volume_changed;
  iface->mute_changed = clapper_control_hub_mute_changed;
  iface->played_item_changed = clapper_control_hub_played_item_changed;
  iface->item_updated = clapper_control_hub_item_updated;
  iface->queue_item_added = clapper_control_hub_queue_item_added;
  iface->queue_item_removed = clapper_control_hub_queue_item_removed;
  iface->queue_item_repositioned = clapper_control_hub_queue_item_repositioned;
  iface->queue_cleared = clapper_control_hub_queue_cleared;
  iface->queue_progression_changed = clapper_control_hub_queue_progression_changed;
}

#define parent_class clapper_control_hub_parent_class
G_DEFINE_TYPE_WITH_CODE (ClapperControlHub, clapper_control_hub, GST_TYPE_OBJECT,
    G_IMPLEMENT_INTERFACE (CLAPPER_TYPE_REACTABLE, clapper_control_hub_reactable_iface_init));

static inline gint
_find_current_port (ClapperControlHub *self)
{
  GSList *uris_list, *list;
  gint found_port = 0;

  uris_list = soup_server_get_uris (self->server);

  for (list = uris_list; list != NULL; list = g_slist_next (list)) {
    GUri *uri = list->data;
    gint current_port = g_uri_get_port (uri);

    if (current_port > 0) {
      found_port = current_port;
      break;
    }
  }

  g_slist_free_full (uris_list, (GDestroyNotify) g_uri_unref);

  if (G_UNLIKELY (found_port == 0))
    GST_ERROR_OBJECT (self, "Could not determine server current port");

  return found_port;
}

static ClapperMediaItem *
_get_item_by_id (ClapperControlHub *self, guint id)
{
  guint i;

  for (i = 0; i < self->items->len; ++i) {
    ClapperMediaItem *item = (ClapperMediaItem *) g_ptr_array_index (self->items, i);

    if (clapper_media_item_get_id (item) == id)
      return item;
  }

  return NULL;
}

static gboolean
_query_parse_item_id (GHashTable *query, guint *id)
{
  const char *id_str;
  gchar *endptr = NULL;

  if (!query || !(id_str = g_hash_table_lookup (query, "id")))
    return FALSE;

  *id = (guint) g_ascii_strtoull (id_str, &endptr, 10);

  return (endptr && *endptr == '\0');
}

static gboolean
_query_get_boolean_value (GHashTable *query, const gchar *key)
{
  const gchar *val;

  if (!(val = g_hash_table_lookup (query, key)))
    return FALSE;

  return (g_ascii_strcasecmp (val, "true") == 0 || strcmp (val, "1") == 0);
}

static void
_item_info_request_cb (SoupServer *server, SoupServerMessage *msg,
    const gchar *path, GHashTable *query, ClapperControlHub *self)
{
  ClapperMediaItem *item;
  gchar *data;
  guint id;
  gboolean with_timeline;

  if (!_query_parse_item_id (query, &id)) {
    soup_server_message_set_status (msg, SOUP_STATUS_BAD_REQUEST, NULL);
    return;
  }
  if (!(item = _get_item_by_id (self, id))) {
    soup_server_message_set_status (msg, SOUP_STATUS_NO_CONTENT, NULL);
    return;
  }

  with_timeline = _query_get_boolean_value (query, "timeline");

  if (!(data = clapper_control_hub_json_build_item_info (self, item, with_timeline))) {
    soup_server_message_set_status (msg, SOUP_STATUS_SERVICE_UNAVAILABLE, NULL);
    return;
  }

  soup_server_message_set_status (msg, SOUP_STATUS_OK, NULL);
  soup_server_message_set_response (msg, "application/json",
      SOUP_MEMORY_TAKE, data, strlen (data));
}

static void
_item_tags_request_cb (SoupServer *server, SoupServerMessage *msg,
    const gchar *path, GHashTable *query, ClapperControlHub *self)
{
  ClapperMediaItem *item;
  GstTagList *tags;
  gchar *data;
  guint id;

  if (!_query_parse_item_id (query, &id)) {
    soup_server_message_set_status (msg, SOUP_STATUS_BAD_REQUEST, NULL);
    return;
  }
  if (!(item = _get_item_by_id (self, id))) {
    soup_server_message_set_status (msg, SOUP_STATUS_NO_CONTENT, NULL);
    return;
  }

  tags = clapper_media_item_get_tags (item);
  data = gst_tag_list_to_string (tags);
  gst_tag_list_unref (tags);

  if (G_UNLIKELY (!data)) {
    soup_server_message_set_status (msg, SOUP_STATUS_INTERNAL_SERVER_ERROR, NULL);
    return;
  }

  soup_server_message_set_status (msg, SOUP_STATUS_OK, NULL);
  soup_server_message_set_response (msg, "application/octet-stream",
      SOUP_MEMORY_TAKE, data, strlen (data));
}

static void
_default_request_cb (SoupServer *server, SoupServerMessage *msg,
    const gchar *path, GHashTable *query, ClapperControlHub *self)
{
  gchar *data;

  if (!(data = clapper_control_hub_json_build_default (self, FALSE))) {
    soup_server_message_set_status (msg, SOUP_STATUS_SERVICE_UNAVAILABLE, NULL);
    return;
  }

  soup_server_message_set_status (msg, SOUP_STATUS_OK, NULL);
  soup_server_message_set_response (msg, "application/json",
      SOUP_MEMORY_TAKE, data, strlen (data));
}

static void
_start_serving (ClapperControlHub *self)
{
  GError *error = NULL;

  if (self->running)
    return;

  if ((self->running = soup_server_listen_all (self->server,
      0, SOUP_SERVER_LISTEN_IPV4_ONLY, &error))) {
    gint port = _find_current_port (self);
    GST_INFO_OBJECT (self, "Server started on port: %i", port);

    if (G_LIKELY (port > 0)) {
      /* Lazy create MDNS, so we do not spin another
       * thread when enhancer is disabled */
      if (!self->mdns) {
        self->mdns = clapper_control_hub_mdns_new ();
        gst_object_set_parent (GST_OBJECT_CAST (self->mdns), GST_OBJECT_CAST (self));
      }

      clapper_control_hub_mdns_start (self->mdns, port);
    }
  } else if (error) {
    ClapperPlayer *player;

    GST_ERROR_OBJECT (self, "Error starting server: %s",
        GST_STR_NULL (error->message));

    if ((player = clapper_reactable_get_player (CLAPPER_REACTABLE_CAST (self)))) {
      GstStructure *structure;

      structure = gst_structure_new ("enhancer-error",
          "error", G_TYPE_ERROR, error, NULL);
      clapper_player_post_message (player,
          gst_message_new_application (GST_OBJECT_CAST (self), structure),
          CLAPPER_PLAYER_MESSAGE_DESTINATION_APPLICATION);

      gst_object_unref (player);
    }

    g_error_free (error);
  }
}

static void
_stop_serving (ClapperControlHub *self)
{
  if (!self->running)
    return;

  clapper_control_hub_mdns_stop (self->mdns);
  soup_server_disconnect (self->server);
  GST_INFO_OBJECT (self, "Server stopped");
  self->running = FALSE;
}

static void
clapper_control_hub_set_active (ClapperControlHub *self, gboolean active)
{
  if (self->active == active)
    return; // No change

  self->active = active;
  (self->active) ? _start_serving (self) : _stop_serving (self);
}

static void
clapper_control_hub_init (ClapperControlHub *self)
{
  self->active = DEFAULT_ACTIVE;
  self->queue_controllable = DEFAULT_QUEUE_CONTROLLABLE;

  /* Player non-zero defaults */
  self->speed = 1.0;
  self->volume = 1.0;

  self->server = soup_server_new ("server-header", "ClapperControlHub", NULL);
  self->ws_connections = g_ptr_array_new_with_free_func ((GDestroyNotify) g_object_unref);

  self->items = g_ptr_array_new_with_free_func ((GDestroyNotify) gst_object_unref);
  self->played_index = CLAPPER_QUEUE_INVALID_POSITION;

  soup_server_add_handler (self->server, "/item", (SoupServerCallback) _item_info_request_cb, self, NULL);
  soup_server_add_handler (self->server, "/tags", (SoupServerCallback) _item_tags_request_cb, self, NULL);
  soup_server_add_handler (self->server, "/", (SoupServerCallback) _default_request_cb, self, NULL);
  soup_server_add_websocket_handler (self->server, "/websocket", NULL, NULL,
      (SoupServerWebsocketCallback) clapper_control_hub_ws_connection_cb, self, NULL);
}

static void
clapper_control_hub_dispose (GObject *object)
{
  ClapperControlHub *self = CLAPPER_CONTROL_HUB_CAST (object);

  _stop_serving (self);
  _clear_stored_queue (self);

  if (self->mdns) {
    gst_object_unparent (GST_OBJECT_CAST (self->mdns));
    gst_clear_object (&self->mdns);
  }
  g_clear_object (&self->server);

  G_OBJECT_CLASS (parent_class)->dispose (object);
}

static void
clapper_control_hub_finalize (GObject *object)
{
  ClapperControlHub *self = CLAPPER_CONTROL_HUB_CAST (object);

  GST_TRACE_OBJECT (self, "Finalize");

  g_ptr_array_unref (self->ws_connections);
  g_ptr_array_unref (self->items);

  G_OBJECT_CLASS (parent_class)->finalize (object);
}

static void
clapper_control_hub_set_property (GObject *object, guint prop_id,
    const GValue *value, GParamSpec *pspec)
{
  ClapperControlHub *self = CLAPPER_CONTROL_HUB_CAST (object);

  switch (prop_id) {
    case PROP_ACTIVE:
      clapper_control_hub_set_active (self, g_value_get_boolean (value));
      break;
    case PROP_QUEUE_CONTROLLABLE:
      self->queue_controllable = g_value_get_boolean (value);
      break;
    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
      break;
  }
}

static void
clapper_control_hub_get_property (GObject *object, guint prop_id,
    GValue *value, GParamSpec *pspec)
{
  ClapperControlHub *self = CLAPPER_CONTROL_HUB_CAST (object);

  switch (prop_id) {
    case PROP_ACTIVE:
      g_value_set_boolean (value, self->active);
      break;
    case PROP_QUEUE_CONTROLLABLE:
      g_value_set_boolean (value, self->queue_controllable);
      break;
    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
      break;
  }
}

static void
clapper_control_hub_class_init (ClapperControlHubClass *klass)
{
  GObjectClass *gobject_class = (GObjectClass *) klass;

  GST_DEBUG_CATEGORY_INIT (GST_CAT_DEFAULT, "clappercontrolhub", 0,
      "Clapper Control Hub");
  clapper_control_hub_ws_debug_init ();

  gobject_class->get_property = clapper_control_hub_get_property;
  gobject_class->set_property = clapper_control_hub_set_property;
  gobject_class->dispose = clapper_control_hub_dispose;
  gobject_class->finalize = clapper_control_hub_finalize;

  /**
   * ClapperControlHub:active:
   *
   * Whether server will run and be discoverable on local network.
   */
  param_specs[PROP_ACTIVE] = g_param_spec_boolean ("active",
      "Active", "Whether server will run and be discoverable on local network", DEFAULT_ACTIVE,
      G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS | CLAPPER_ENHANCER_PARAM_GLOBAL | CLAPPER_ENHANCER_PARAM_LOCAL);

  /**
   * ClapperControlHub:queue-controllable:
   *
   * Whether remote clients can control playback queue.
   *
   * This includes ability to open new URIs, adding/removing items from
   * the queue and selecting current item for playback remotely.
   *
   * You probably want to keep this disabled if your application
   * is supposed to manage what is played now and not the client.
   */
  param_specs[PROP_QUEUE_CONTROLLABLE] = g_param_spec_boolean ("queue-controllable",
      NULL, NULL, DEFAULT_QUEUE_CONTROLLABLE,
      G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS | CLAPPER_ENHANCER_PARAM_LOCAL);

  g_object_class_install_properties (gobject_class, PROP_LAST, param_specs);
}

void
peas_register_types (PeasObjectModule *module)
{
  peas_object_module_register_extension_type (module, CLAPPER_TYPE_REACTABLE, CLAPPER_TYPE_CONTROL_HUB);
}
