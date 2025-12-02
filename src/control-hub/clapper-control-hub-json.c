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

#include "clapper-control-hub-json.h"

#define _JSON_BUILD(dest,...) {                                \
    GString *_json = g_string_new ("{");                       \
    __VA_ARGS__                                                \
    g_string_append (_json, "}");                              \
    *dest = g_string_free (_json, FALSE); }

#define _JSON_AUTO_COMMA {                                     \
    const guchar _last = _json->str[_json->len - 1];           \
    if (_last != '{' && _last != '[')                          \
      g_string_append (_json, ","); }

#define _ADD_KEY_VAL(k,t,v) {                                  \
    _JSON_AUTO_COMMA                                           \
    g_string_append_printf (_json, "\"" k "\":" t, v); }

#define _ADD_VAL(t,v) {                                        \
    _JSON_AUTO_COMMA                                           \
    g_string_append_printf (_json, t, v); }

#define _ADD_OBJECT(...) {                                     \
    _JSON_AUTO_COMMA                                           \
    g_string_append (_json, "{");                              \
    __VA_ARGS__                                                \
    g_string_append (_json, "}"); }

#define _ADD_NAMED_OBJECT(name,...) {                          \
    _JSON_AUTO_COMMA                                           \
    g_string_append_printf (_json, "\"%s\":{", name);          \
    __VA_ARGS__                                                \
    g_string_append (_json, "}"); }

#define _ADD_NAMED_ARRAY(name,...) {                           \
    _JSON_AUTO_COMMA                                           \
    g_string_append_printf (_json, "\"%s\":[", name);          \
    __VA_ARGS__                                                \
    g_string_append (_json, "]"); }

static inline void
clapper_server_json_escape_string (gchar **string)
{
  const gchar *src = *string;
  guint offset = 0;

  while ((src = strchr (src, '"')) != NULL) {
    ++offset;
    ++src;
  }

  /* Needs escaping for JSON */
  if (offset > 0) {
    gchar *escaped, *dst;
    src = *string;

    /* Previous length + n_escapes + term */
    escaped = g_new (gchar, strlen (src) + offset + 1);
    dst = escaped;

    while (*src) {
      if (*src == '"') {
        *dst++ = '\\';
        *dst++ = '"';
      } else {
        *dst++ = *src;
      }
      ++src;
    }
    *dst = '\0';

    g_free (*string);
    *string = escaped;
  }
}

gchar *
clapper_control_hub_json_build_default (ClapperControlHub *hub, gboolean as_event)
{
  gchar *data;

  _JSON_BUILD (&data, {
    if (as_event)
      _ADD_KEY_VAL ("event", "\"%s\"", "snapshot");
    _ADD_KEY_VAL ("state", "%u", hub->state);
    _ADD_KEY_VAL ("position", "%.3lf", hub->position);
    _ADD_KEY_VAL ("speed", "%.2lf", hub->speed);
    _ADD_KEY_VAL ("volume", "%.2lf", hub->volume);
    _ADD_KEY_VAL ("mute", "%s", hub->mute ? "true" : "false");
    _ADD_NAMED_OBJECT ("queue", {
      _ADD_KEY_VAL ("controllable", "%s", hub->queue_controllable ? "true" : "false");
      _ADD_KEY_VAL ("progression", "%u", hub->progression);
      _ADD_KEY_VAL ("played_index", "%u", hub->played_index);
      _ADD_NAMED_ARRAY ("items", {
        guint i;
        for (i = 0; i < hub->items->len; ++i) {
          _ADD_OBJECT ({
            ClapperMediaItem *item = (ClapperMediaItem *) g_ptr_array_index (hub->items, i);
            gchar *title = clapper_media_item_get_title (item);
            clapper_server_json_escape_string (&title);
            _ADD_KEY_VAL ("id", "%u", clapper_media_item_get_id (item));
            _ADD_KEY_VAL ("title", "\"%s\"", title);
            _ADD_KEY_VAL ("duration", "%.3lf", clapper_media_item_get_duration (item));
            g_free (title);
          });
        }
      });
    });
  });

  return data;
}

gchar *
clapper_control_hub_json_build_item_info (ClapperControlHub *hub, ClapperMediaItem *item, gboolean with_timeline)
{
  gchar *data, *title;

  title = clapper_media_item_get_title (item);
  clapper_server_json_escape_string (&title);

  _JSON_BUILD (&data, {
    _ADD_KEY_VAL ("id", "%u", clapper_media_item_get_id (item));
    _ADD_KEY_VAL ("title", "\"%s\"", title);
    _ADD_KEY_VAL ("duration", "%.3lf", clapper_media_item_get_duration (item));
    if (with_timeline) {
      _ADD_NAMED_ARRAY ("timeline", {
        ClapperTimeline *timeline = clapper_media_item_get_timeline (item);
        guint i, n_markers = clapper_timeline_get_n_markers (timeline);
        for (i = 0; i < n_markers; ++i) {
          _ADD_OBJECT ({
            ClapperMarker *marker;
            if ((marker = clapper_timeline_get_marker (timeline, i))) {
              _ADD_KEY_VAL ("marker_type", "%u", clapper_marker_get_marker_type (marker));
              _ADD_KEY_VAL ("title", "\"%s\"", clapper_marker_get_title (marker));
              _ADD_KEY_VAL ("start", "%.3lf", clapper_marker_get_start (marker));
              _ADD_KEY_VAL ("end", "%.3lf", clapper_marker_get_end (marker));
              gst_object_unref (marker);
            }
          });
        }
      });
    }
  });

  g_free (title);

  return data;
}
