/* Clapper Enhancer LBRY
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
 * License along with this library; if not, see
 * <https://www.gnu.org/licenses/>.
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

#include "../utils/c/json/json-utils.h"

#define LBRY_API_URI "https://api.na-backend.odysee.com/api/v1/proxy"

#define GST_CAT_DEFAULT clapper_lbry_debug
GST_DEBUG_CATEGORY_STATIC (GST_CAT_DEFAULT);

#define CLAPPER_TYPE_LBRY (clapper_lbry_get_type())
#define CLAPPER_LBRY_CAST(obj) ((ClapperLbry *)(obj))
G_DECLARE_FINAL_TYPE (ClapperLbry, clapper_lbry, CLAPPER, LBRY, GstObject);

G_MODULE_EXPORT void peas_register_types (PeasObjectModule *module);

typedef enum
{
  LBRY_STEP_GET,
  LBRY_STEP_RESOLVE,
  LBRY_STEP_FINISH,
} LbryStep;

struct _ClapperLbry
{
  GstObject parent;

  SoupSession *session;

  gchar *video_id;
  gchar *streaming_url;
};

static inline SoupMessage *
_make_api_message (ClapperLbry *self, LbryStep step)
{
  SoupMessage *msg;
  SoupMessageHeaders *headers;
  GBytes *bytes;
  gchar *req_body;

  msg = soup_message_new ("POST", LBRY_API_URI);

  JSON_UTILS_BUILD_OBJECT (&req_body, {
    JSON_UTILS_ADD_KEY_VAL_STRING ("method", (step == LBRY_STEP_GET) ? "get" : "resolve");
    JSON_UTILS_ADD_NAMED_OBJECT ("params", {
      JSON_UTILS_ADD_KEY_VAL_STRING ((step == LBRY_STEP_GET) ? "uri" : "urls", self->video_id);
    });
  });

  bytes = g_bytes_new_take (req_body, strlen (req_body));
  soup_message_set_request_body_from_bytes (msg, "application/json-rpc", bytes);
  g_bytes_unref (bytes);

  headers = soup_message_get_request_headers (msg);
  soup_message_headers_replace (headers, "Origin", "https://odysee.com");
  soup_message_headers_replace (headers, "Referer", "https://odysee.com/");

  return msg;
}

static gboolean
_acquire_streaming_url (ClapperLbry *self, JsonReader *reader,
    GCancellable *cancellable, GError **error)
{
  GST_DEBUG_OBJECT (self, "Searching for streaming URL...");

  if (gst_debug_category_get_threshold (GST_CAT_DEFAULT) >= GST_LEVEL_LOG) {
    gchar *data = json_utils_reader_to_string (reader, TRUE);
    GST_LOG_OBJECT (self, "API \"get\" response:\n%s", data);
    g_free (data);
  }

  self->streaming_url = g_strdup (json_utils_get_string (reader, "result", "streaming_url", NULL));

  if (!self->streaming_url) {
    g_set_error (error, G_IO_ERROR, G_IO_ERROR_FAILED,
        "LBRY streaming URL is missing");
    return FALSE;
  }

  GST_DEBUG_OBJECT (self, "Got streaming URL: %s", self->streaming_url);

  return TRUE;
}

static gboolean
_fill_harvest (ClapperLbry *self, JsonReader *reader, ClapperHarvest *harvest,
    GCancellable *cancellable, GError **error)
{
  GST_DEBUG_OBJECT (self, "Harvesting...");

  if (gst_debug_category_get_threshold (GST_CAT_DEFAULT) >= GST_LEVEL_LOG) {
    gchar *data = json_utils_reader_to_string (reader, TRUE);
    GST_LOG_OBJECT (self, "API \"resolve\" response:\n%s", data);
    g_free (data);
  }

  if (!json_utils_go_to (reader, "result", self->video_id, "value", NULL)) {
    g_set_error (error, G_IO_ERROR, G_IO_ERROR_FAILED,
        "Invalid LBRY \"resolve\" API response");
    return FALSE;
  }

  clapper_harvest_tags_add (harvest,
      GST_TAG_TITLE, json_utils_get_string (reader, "title", NULL),
      GST_TAG_DURATION, json_utils_get_int (reader, "video", "duration", NULL) * GST_SECOND,
      NULL);

  /* Return from "result.video_id.value" */
  json_utils_go_back (reader, 3);

  clapper_harvest_headers_set (harvest,
      "Origin", "https://odysee.com",
      "Referer", "https://odysee.com/",
      NULL);

  /* Harvest takes data (transfer full) */
  clapper_harvest_fill_with_text (harvest, "text/x-uri", self->streaming_url);
  self->streaming_url = NULL; // safety

  GST_DEBUG_OBJECT (self, "Harvest done");

  return TRUE;
}

static gboolean
clapper_lbry_extract (ClapperExtractable *extractable, GUri *uri, ClapperHarvest *harvest,
    GCancellable *cancellable, GError **error)
{
  ClapperLbry *self = CLAPPER_LBRY_CAST (extractable);
  LbryStep step = LBRY_STEP_GET;
  SoupMessage *msg;
  SoupStatus status;
  GInputStream *stream;
  JsonReader *reader;
  gboolean success;

  GST_DEBUG_OBJECT (self, "Extract");

  /* Always use "lbry" scheme */
  if (strcmp (g_uri_get_scheme (uri), "lbry") == 0) {
    self->video_id = g_uri_to_string (uri);
  } else {
    self->video_id = g_uri_join (G_URI_FLAGS_ENCODED, "lbry",
        NULL, g_uri_get_path (uri) + 1, -1, "",
        NULL, g_uri_get_fragment (uri));
  }

  GST_DEBUG_OBJECT (self, "Requested video: %s", self->video_id);

start_step:
  success = FALSE;

  msg = _make_api_message (self, step);
  stream = soup_session_send (self->session, msg, cancellable, error);
  g_object_unref (msg);

  /* Could not send message */
  if (!stream)
    goto finish_step;

  if ((status = soup_message_get_status (msg)) >= 400) {
    g_set_error (error, G_IO_ERROR, G_IO_ERROR_FAILED,
        "HTTP response code: %i", status);
    goto finish_step;
  }

  if (!(reader = json_utils_read_stream (stream, cancellable, error)))
    goto finish_step;

  switch (step) {
    case LBRY_STEP_GET:
      success = _acquire_streaming_url (self, reader, cancellable, error);
      break;
    case LBRY_STEP_RESOLVE:
      success = _fill_harvest (self, reader, harvest, cancellable, error);
      break;
    default:
      g_assert_not_reached ();
      break;
  }

finish_step:
  g_clear_object (&reader);
  g_clear_object (&stream);

  /* Check if not cancelled before going to next step */
  if (success)
    success = !g_cancellable_is_cancelled (cancellable);

  /* Step done, go to next */
  if (success && ++step < LBRY_STEP_FINISH)
    goto start_step;

  GST_DEBUG_OBJECT (self, "Extraction %s", (success) ? "succeded" : "failed");

  return success;
}

static void
clapper_lbry_extractable_iface_init (ClapperExtractableInterface *iface)
{
  iface->extract = clapper_lbry_extract;
}

#define parent_class clapper_lbry_parent_class
G_DEFINE_TYPE_WITH_CODE (ClapperLbry, clapper_lbry, GST_TYPE_OBJECT,
    G_IMPLEMENT_INTERFACE (CLAPPER_TYPE_EXTRACTABLE, clapper_lbry_extractable_iface_init));

static void
clapper_lbry_init (ClapperLbry *self)
{
  self->session = soup_session_new_with_options ("timeout", 7, NULL);
}

static void
clapper_lbry_finalize (GObject *object)
{
  ClapperLbry *self = CLAPPER_LBRY_CAST (object);

  GST_TRACE_OBJECT (self, "Finalize");

  g_object_unref (self->session);

  g_free (self->streaming_url);
  g_free (self->video_id);

  G_OBJECT_CLASS (parent_class)->finalize (object);
}

static void
clapper_lbry_class_init (ClapperLbryClass *klass)
{
  GObjectClass *gobject_class = (GObjectClass *) klass;

  GST_DEBUG_CATEGORY_INIT (GST_CAT_DEFAULT, "clapperlbry", 0,
      "Clapper LBRY");

  gobject_class->finalize = clapper_lbry_finalize;
}

void
peas_register_types (PeasObjectModule *module)
{
  peas_object_module_register_extension_type (module, CLAPPER_TYPE_EXTRACTABLE, CLAPPER_TYPE_LBRY);
}
