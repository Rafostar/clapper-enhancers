/* Clapper Enhancer PeerTube
 * Copyright (C) 2024 Rafał Dzięgiel <rafostar.github@gmail.com>
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
 * License along with this library; if not, write to the
 * Free Software Foundation, Inc., 51 Franklin St, Fifth Floor,
 * Boston, MA 02110-1301, USA.
 */

#include <glib.h>
#include <glib-object.h>
#include <gio/gio.h>
#include <gmodule.h>
#include <libpeas.h>
#include <clapper/clapper.h>

#include <gst/gst.h>
#include <gst/tag/tag.h>
#include <libsoup/soup.h>

#include "../utils/c/common/common-utils.h"
#include "../utils/c/json/json-utils.h"

#define GST_CAT_DEFAULT clapper_peertube_debug
GST_DEBUG_CATEGORY_STATIC (GST_CAT_DEFAULT);

#define CLAPPER_TYPE_PEERTUBE (clapper_peertube_get_type())
#define CLAPPER_PEERTUBE_CAST(obj) ((ClapperPeertube *)(obj))
G_DECLARE_FINAL_TYPE (ClapperPeertube, clapper_peertube, CLAPPER, PEERTUBE, GstObject);

G_MODULE_EXPORT void peas_register_types (PeasObjectModule *module);

struct _ClapperPeertube
{
  GstObject parent;

  SoupSession *session;
};

static inline SoupMessage *
_make_api_message (ClapperPeertube *self, GUri *uri, const gchar *video_id)
{
  SoupMessage *msg;
  GUri *dest_uri;
  gchar *path;
  gboolean use_http;

  use_http = (g_uri_get_port (uri) == 80 || strcmp (g_uri_get_scheme (uri), "http") == 0);

  GST_DEBUG_OBJECT (self, "Using secure HTTP: %s", use_http ? "no" : "yes");

  path = g_strdup_printf ("/api/v1/videos/%s", video_id);
  dest_uri = g_uri_build (G_URI_FLAGS_ENCODED,
      (use_http) ? "http" : "https", NULL,
      g_uri_get_host (uri),
      g_uri_get_port (uri),
      path, NULL, NULL);
  g_free (path);

  msg = soup_message_new_from_uri ("GET", dest_uri);
  g_uri_unref (dest_uri);

  return msg;
}

static gboolean
_read_uris_array_cb (JsonReader *reader, ClapperHarvest *harvest, const gchar *key_str)
{
  const gchar *uri;
  gboolean filled = FALSE;

  if ((uri = json_utils_get_string (reader, key_str, NULL)))
    filled = clapper_harvest_fill_with_text (harvest, "text/uri-list", g_strdup (uri));

  return !filled;
}

static inline gboolean
_read_extract_api_response (ClapperPeertube *self, JsonReader *reader, ClapperHarvest *harvest, GError **error)
{
  gboolean success = FALSE;

  if (gst_debug_category_get_threshold (GST_CAT_DEFAULT) >= GST_LEVEL_DEBUG) {
    gchar *data = json_utils_reader_to_string (reader, TRUE);
    GST_DEBUG_OBJECT (self, "API response:\n%s", data);
    g_free (data);
  }

  clapper_harvest_tags_add (harvest,
      GST_TAG_TITLE, json_utils_get_string (reader, "name", NULL),
      GST_TAG_DURATION, json_utils_get_int (reader, "duration", NULL) * GST_SECOND,
      NULL);

  if (json_utils_go_to (reader, "streamingPlaylists", NULL)) {
    const gchar *key_str = "playlistUrl";
    success = json_utils_array_foreach (reader, harvest,
        (JsonUtilsForeachFunc) _read_uris_array_cb, (gpointer) key_str);
    json_utils_go_back (reader, 1);
  }
  if (!success && json_utils_go_to (reader, "files", NULL)) {
    const gchar *key_str = "fileUrl";
    success = json_utils_array_foreach (reader, harvest,
        (JsonUtilsForeachFunc) _read_uris_array_cb, (gpointer) key_str);
    json_utils_go_back (reader, 1);
  }

  return success;
}

static gboolean
clapper_peertube_extract (ClapperExtractable *extractable, GUri *uri, ClapperHarvest *harvest,
    GCancellable *cancellable, GError **error)
{
  ClapperPeertube *self = CLAPPER_PEERTUBE_CAST (extractable);
  SoupMessage *msg;
  GInputStream *stream;
  JsonReader *reader;
  gchar *video_id;
  gboolean success = FALSE;

  GST_DEBUG_OBJECT (self, "Extract");

  if (!(video_id = common_utils_match_regex (
      "/(?:videos/(?:watch|embed)|w)/([A-Za-z0-9]+)", g_uri_get_path (uri)))) {
    g_set_error (error, G_IO_ERROR, G_IO_ERROR_FAILED,
        "Could not determine video ID from URI");
    return FALSE;
  }

  GST_DEBUG_OBJECT (self, "Creating API request");
  msg = _make_api_message (self, uri, video_id);
  g_free (video_id);

  stream = soup_session_send (self->session, msg, cancellable, error);
  g_object_unref (msg);

  /* Could not send message */
  if (!stream)
    return FALSE;

  GST_LOG_OBJECT (self, "Loading response data");

  if ((reader = json_utils_read_stream (stream, cancellable, error))) {
    GST_DEBUG_OBJECT (self, "Reading response");
    success = _read_extract_api_response (self, reader, harvest, error);
    g_object_unref (reader);
  }

  if (G_UNLIKELY (!g_input_stream_close (stream, NULL, NULL)))
    GST_ERROR_OBJECT (self, "Could not close input stream!");

  g_object_unref (stream);

  GST_DEBUG_OBJECT (self, "Extraction %s", (success) ? "succeded" : "failed");

  return success;
}

static void
clapper_peertube_extractable_iface_init (ClapperExtractableInterface *iface)
{
  iface->extract = clapper_peertube_extract;
}

#define parent_class clapper_peertube_parent_class
G_DEFINE_TYPE_WITH_CODE (ClapperPeertube, clapper_peertube, GST_TYPE_OBJECT,
    G_IMPLEMENT_INTERFACE (CLAPPER_TYPE_EXTRACTABLE, clapper_peertube_extractable_iface_init));

static void
clapper_peertube_init (ClapperPeertube *self)
{
  self->session = soup_session_new_with_options ("timeout", 7, NULL);
}

static void
clapper_peertube_finalize (GObject *object)
{
  ClapperPeertube *self = CLAPPER_PEERTUBE_CAST (object);

  GST_TRACE_OBJECT (self, "Finalize");

  g_object_unref (self->session);

  G_OBJECT_CLASS (parent_class)->finalize (object);
}

static void
clapper_peertube_class_init (ClapperPeertubeClass *klass)
{
  GObjectClass *gobject_class = (GObjectClass *) klass;

  GST_DEBUG_CATEGORY_INIT (GST_CAT_DEFAULT, "clapperpeertube", 0,
      "Clapper PeerTube");

  gobject_class->finalize = clapper_peertube_finalize;
}

void
peas_register_types (PeasObjectModule *module)
{
  peas_object_module_register_extension_type (module, CLAPPER_TYPE_EXTRACTABLE, CLAPPER_TYPE_PEERTUBE);
}
