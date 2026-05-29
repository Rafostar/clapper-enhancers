/* Clapper Enhancer Subber
 * Copyright (C) 2026 Rafał Dzięgiel <rafostar.github@gmail.com>
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

#include "config.h"

#include <glib.h>
#include <glib-object.h>
#include <gio/gio.h>
#include <gmodule.h>
#include <libpeas.h>
#include <clapper/clapper.h>

#include <libsoup/soup.h>

#include "clapper-subber-ranker.h"
#include "clapper-subber.gresources.h"
#include "../utils/c/common/common-utils.h"
#include "../utils/c/json/json-utils.h"

/* Values from OpenSubtitles API documentation */
#define LOWER_BOUND_FILE_SIZE 131072
#define UPPER_BOUND_FILE_SIZE 9000000000
#define CHUNK_SIZE 65536 // 64KB

#define DEFAULT_AI_TRANSLATED CLAPPER_SUBBER_INCLUSION_EXCLUDE
#define DEFAULT_HEARING_IMPAIRED CLAPPER_SUBBER_INCLUSION_EXCLUDE
#define DEFAULT_MACHINE_TRANSLATED CLAPPER_SUBBER_INCLUSION_EXCLUDE

#define GST_CAT_DEFAULT clapper_subber_debug
GST_DEBUG_CATEGORY_STATIC (GST_CAT_DEFAULT);

#define CLAPPER_TYPE_SUBBER (clapper_subber_get_type())
#define CLAPPER_SUBBER_CAST(obj) ((ClapperSubber *)(obj))
G_DECLARE_FINAL_TYPE (ClapperSubber, clapper_subber, CLAPPER, SUBBER, GstObject);

G_MODULE_EXPORT void peas_register_types (PeasObjectModule *module);

typedef enum
{
  CLAPPER_SUBBER_INCLUSION_EXCLUDE = 0,
  CLAPPER_SUBBER_INCLUSION_INCLUDE,
  CLAPPER_SUBBER_INCLUSION_ONLY
} ClapperSubberInclusion;

#define CLAPPER_SUBBER_TYPE_INCLUSION (clapper_subber_inclusion_get_type ())
COMMON_UTILS_DEFINE_ENUM_TYPE (ClapperSubberInclusion, clapper_subber_inclusion,
    G_DEFINE_ENUM_VALUE (CLAPPER_SUBBER_INCLUSION_EXCLUDE, "exclude"),
    G_DEFINE_ENUM_VALUE (CLAPPER_SUBBER_INCLUSION_INCLUDE, "include"),
    G_DEFINE_ENUM_VALUE (CLAPPER_SUBBER_INCLUSION_ONLY, "only"));

#define CLAPPER_SUBBER_TYPE_INCLUSION_SIMPLE (clapper_subber_inclusion_simple_get_type ())
COMMON_UTILS_DEFINE_ENUM_TYPE (ClapperSubberInclusionSimple, clapper_subber_inclusion_simple,
    G_DEFINE_ENUM_VALUE (CLAPPER_SUBBER_INCLUSION_EXCLUDE, "exclude"),
    G_DEFINE_ENUM_VALUE (CLAPPER_SUBBER_INCLUSION_INCLUDE, "include"));

struct _ClapperSubber
{
  GstObject parent;

  GPtrArray *active_downloads;
  GCancellable *cancellable;

  ClapperMediaItem *played_item;

  gchar *consumer;

  gchar *base_url;
  gchar *token;
  gboolean logged_in;

  gchar *username;
  gchar *password;
  gchar *langs_order;
  ClapperSubberInclusionSimple ai_translated;
  ClapperSubberInclusion hearing_impaired;
  ClapperSubberInclusionSimple machine_translated;
};

enum
{
  PROP_0,
  PROP_OPENSUBTITLES_USERNAME,
  PROP_OPENSUBTITLES_PASSWORD,
  PROP_LANGUAGES_ORDER,
  PROP_AI_TRANSLATED,
  PROP_HEARING_IMPAIRED,
  PROP_MACHINE_TRANSLATED,
  PROP_LAST
};

static GParamSpec *param_specs[PROP_LAST] = { NULL, };

static const gchar *
_get_inclusion_string (gint inclusion)
{
  switch (inclusion) {
    case CLAPPER_SUBBER_INCLUSION_ONLY:
      return "only";
    case CLAPPER_SUBBER_INCLUSION_INCLUDE:
      return "include";
    case CLAPPER_SUBBER_INCLUSION_EXCLUDE:
    default:
      return "exclude";
  }
}

static void
_ensure_consumer (ClapperSubber *self)
{
  GResource *resource;
  GBytes *bytes;
  gchar *data;
  gsize data_len = 0;

  if (self->consumer)
    return;

  resource = clapper_subber_get_resource ();
  bytes = g_resource_lookup_data (resource,
      CLAPPER_SUBBER_RESOURCE_PATH "/consumer",
      G_RESOURCE_LOOKUP_FLAGS_NONE, NULL);
  data = g_bytes_unref_to_data (bytes, &data_len);
  self->consumer = g_strndup (data, data_len);
  g_free (data);
}

static gboolean
_is_supported_proto (const gchar *uri)
{
  const gchar *file_protos[] = { "file", "sftp", "ftp", "smb", "davs", "dav" };
  guint i;

  for (i = 0; i < G_N_ELEMENTS (file_protos); ++i) {
    if (gst_uri_has_protocol (uri, file_protos[i])) {
      return TRUE;
    }
  }

  return FALSE;
}

static inline gchar *
_generate_data_hash (ClapperSubber *self, const gchar *uri,
    GCancellable *cancellable, GError **error)
{
  gchar *hash_str = NULL;
  guint64 hash = 0;

  GFile *file;
  GFileInputStream *istream;
  GFileInfo *info;
  gsize file_size = 0;

  GST_DEBUG_OBJECT (self, "Generating hash: \"%s\"", uri);

  file = g_file_new_for_uri (uri);

  if ((info = g_file_query_info (file,
      G_FILE_ATTRIBUTE_STANDARD_SIZE,
      G_FILE_QUERY_INFO_NONE, cancellable, error))) {
    file_size = g_file_info_get_size (info);
    g_object_unref (info);
  } else {
    g_object_unref (file);
    return NULL;
  }

  GST_DEBUG_OBJECT (self, "File size: %" G_GSIZE_FORMAT, file_size);

  /* NOTE: OpenSubtitles file size restriction */
  if (file_size <= LOWER_BOUND_FILE_SIZE || file_size >= UPPER_BOUND_FILE_SIZE) {
    g_set_error (error, G_IO_ERROR, G_IO_ERROR_FAILED, "File size is unsupported");
    g_object_unref (file);
    return NULL;
  }

  if (!(istream = g_file_read (file, cancellable, error))) {
    g_object_unref (file);
    return NULL;
  }

  if (g_seekable_can_seek (G_SEEKABLE (istream))) {
    const gsize offsets[] = { 0, file_size - CHUNK_SIZE };
    guint8 buffer[CHUNK_SIZE];
    guint i;
    gboolean success = TRUE;

    hash += file_size;

    for (i = 0; i < G_N_ELEMENTS (offsets); ++i) {
      gsize bytes_read;

      GST_LOG_OBJECT (self, "Reading file at offset: %" G_GSIZE_FORMAT, offsets[i]);

      if (offsets[i] > 0 && !g_seekable_seek (G_SEEKABLE (istream), offsets[i],
          G_SEEK_SET, cancellable, error)) {
        success = FALSE;
        break;
      }

      bytes_read = g_input_stream_read (G_INPUT_STREAM (istream),
          buffer, CHUNK_SIZE, cancellable, error);
      if (*error != NULL) {
        success = FALSE;
        break;
      }

      if (G_LIKELY (bytes_read == CHUNK_SIZE)) {
        const guint64 *buffer64 = (const guint64 *) buffer;
        gsize j, n_elems = CHUNK_SIZE / sizeof (guint64);

        for (j = 0; j < n_elems; ++j)
          hash += GUINT64_FROM_LE (buffer64[j]);
      } else {
        success = FALSE;
        break;
      }
    }

    if (success)
      hash_str = g_strdup_printf ("%016" G_GINT64_MODIFIER "x", hash);
  } else {
    /* Not an error - server might not support seeking */
    GST_DEBUG_OBJECT (self, "File is not seekable");
  }

  g_object_unref (istream);
  g_object_unref (file);

  return hash_str;
}

static gboolean
_download_subtitles_data (ClapperSubber *self, SoupSession *session, const gchar *uri,
    GFile *dl_file, GCancellable *cancellable, GError **error)
{
  SoupMessage *msg;
  SoupMessageHeaders *headers;
  GInputStream *stream;
  gssize data_size = -1;

  msg = soup_message_new ("GET", uri);

  headers = soup_message_get_request_headers (msg);
  soup_message_headers_append (headers, CLAPPER_SUBBER_UA_HEADER, CLAPPER_SUBBER_UA_STRING);
  soup_message_headers_append (headers, CLAPPER_SUBBER_LOGIN_HEADER, self->consumer);

  stream = soup_session_send (session, msg, cancellable, error);
  g_object_unref (msg);

  if (stream) {
    GFileOutputStream *fostream;

    if ((fostream = g_file_replace (dl_file, NULL, FALSE,
        G_FILE_CREATE_NONE, cancellable, error))) {
      data_size = g_output_stream_splice (G_OUTPUT_STREAM (fostream),
          stream, G_OUTPUT_STREAM_SPLICE_CLOSE_SOURCE | G_OUTPUT_STREAM_SPLICE_CLOSE_TARGET,
          cancellable, error);

      g_object_unref(fostream);
    }

    g_object_unref (stream);
  }

  if (data_size < 0) {
    g_file_delete (dl_file, NULL, NULL); // Ensure incomplete file is deleted
    return FALSE;
  }

  return TRUE;
}

static inline GFile *
_get_data_dir (ClapperSubber *self, GCancellable *cancellable, GError **error)
{
  GFile *data_dir = g_file_new_build_filename (g_get_user_data_dir (),
      CLAPPER_API_NAME, "enhancers", "clapper-subber", NULL);

  if (!g_file_make_directory_with_parents (data_dir, cancellable, error)) {
    if ((*error)->domain != G_IO_ERROR || (*error)->code != G_IO_ERROR_EXISTS) {
      GST_ERROR_OBJECT (self, "Failed to create directory for downloads: %s", (*error)->message);
      g_clear_object (&data_dir);
    }
  }

  return data_dir;
}

static GFile *
_opensubtitles_download (ClapperSubber *self, SoupSession *session, gint64 file_id,
    GCancellable *cancellable, GError **error)
{
  GFile *data_dir, *dl_file;
  SoupMessage *msg;
  SoupMessageHeaders *headers;
  GBytes *bytes;
  GInputStream *stream;
  gchar *req_uri, *req_body, *auth, dest_name[32];
  gboolean success;

  if (!(data_dir = _get_data_dir (self, cancellable, error)))
    return NULL; // when failed to create dir

  g_snprintf (dest_name, sizeof (dest_name), "%" G_GINT64_FORMAT, file_id);
  dl_file = g_file_get_child (data_dir, dest_name);

  /* Skip download if downloaded before */
  success = g_file_query_exists (dl_file, cancellable);
  if (success || g_cancellable_is_cancelled (cancellable))
    goto finish;

  req_uri = g_strdup_printf ("https://%s/api/v1/download", self->base_url);
  msg = soup_message_new ("POST", req_uri);
  g_free (req_uri);

  JSON_UTILS_BUILD_OBJECT (&req_body, {
    JSON_UTILS_ADD_KEY_VAL_INT ("file_id", file_id);
  });

  bytes = g_bytes_new_take (req_body, strlen (req_body));
  soup_message_set_request_body_from_bytes (msg, "application/json", bytes);
  g_bytes_unref (bytes);

  auth = g_strdup_printf ("Bearer %s", self->token);

  headers = soup_message_get_request_headers (msg);
  soup_message_headers_append (headers, CLAPPER_SUBBER_UA_HEADER, CLAPPER_SUBBER_UA_STRING);
  soup_message_headers_append (headers, CLAPPER_SUBBER_LOGIN_HEADER, self->consumer);
  soup_message_headers_append (headers, CLAPPER_SUBBER_AUTH_HEADER, auth);
  soup_message_headers_append (headers, CLAPPER_SUBBER_ACCEPT_HEADER, "application/json");

  stream = soup_session_send (session, msg, cancellable, error);
  g_object_unref (msg);

  if (stream) {
    JsonReader *reader;
    const gchar *link;

    if ((reader = json_utils_read_stream (stream, cancellable, error))) {
      if (gst_debug_category_get_threshold (GST_CAT_DEFAULT) >= GST_LEVEL_DEBUG) {
        gchar *json = json_utils_reader_to_string (reader, TRUE);
        GST_DEBUG_OBJECT (self, "Download response:\n%s", json);
        g_free (json);
      }

      if ((link = json_utils_get_string (reader, "link", NULL)))
        success = _download_subtitles_data (self, session, link, dl_file, cancellable, error);

      g_object_unref (reader);
    }

    g_object_unref (stream);
  }

finish:
  if (success) {
    GST_INFO_OBJECT (self, "Subtitles downloaded");
  } else {
    if (*error == NULL) {
      g_set_error (error, G_IO_ERROR, G_IO_ERROR_FAILED,
          "%s", (g_cancellable_is_cancelled (cancellable))
          ? "Subtitles download was cancelled"
          : "Unknown subtitles download error");
    }
    g_clear_object (&dl_file);
  }

  return dl_file;
}

static gchar *
_generate_search_query (ClapperSubber *self, JsonReader *guessit, const gchar *hash)
{
  GData *data;
  gint64 season, episode, year;
  gchar season_str[32], episode_str[32], year_str[32];
  const gchar *name, *type;
  gchar *encoded;

  name = json_utils_get_string (guessit, "title", NULL);
  type = json_utils_get_string (guessit, "type", NULL);

  season = json_utils_get_int (guessit, "season", NULL);
  episode = json_utils_get_int (guessit, "episode", NULL);
  year = json_utils_get_int (guessit, "year", NULL);

  /* Syntax "Season 2 - 06" might be incorrectly parsed
   * as range of seasons (in the form of array) */
  if (season == 0 && episode == 0) {
    gint count = json_utils_count_elements (guessit, "season", NULL);
    if (count > 1) {
      season = json_utils_get_int (guessit, "season", JSON_UTILS_ARRAY_INDEX (0), NULL);
      episode = json_utils_get_int (guessit, "season", JSON_UTILS_ARRAY_INDEX (count - 1), NULL);
    }
  }

  g_datalist_init (&data);

  GST_OBJECT_LOCK (self);

  /* OpenSubtitles asks for keys in alphabetical order with lowercase values */
  g_datalist_set_data (&data, "ai_translated", (gpointer) _get_inclusion_string (self->ai_translated));
  if (episode > 0) {
    g_snprintf (episode_str, sizeof (episode_str), "%" G_GINT64_FORMAT, episode);
    g_datalist_set_data (&data, "episode_number", (gpointer) episode_str);
  }
  g_datalist_set_data (&data, "foreign_parts_only", (gpointer) "exclude"); // Only whole subtitles
  g_datalist_set_data (&data, "hearing_impaired", (gpointer) _get_inclusion_string (self->hearing_impaired));
  if (self->langs_order)
    g_datalist_set_data (&data, "languages", (gpointer) self->langs_order);
  g_datalist_set_data (&data, "machine_translated", (gpointer) _get_inclusion_string (self->machine_translated));
  if (hash)
    g_datalist_set_data (&data, "moviehash", (gpointer) hash);
  if (name)
    g_datalist_set_data_full (&data, "query", g_utf8_strdown (name, -1), g_free); // Lowercase
  if (season > 0) {
    g_snprintf (season_str, sizeof (season_str), "%" G_GINT64_FORMAT, season);
    g_datalist_set_data (&data, "season_number", (gpointer) season_str);
  }
  if (type)
    g_datalist_set_data (&data, "type", (gpointer) type);
  if (year > 0) {
    g_snprintf (year_str, sizeof (year_str), "%" G_GINT64_FORMAT, year);
    g_datalist_set_data (&data, "year", (gpointer) year_str);
  }

  GST_OBJECT_UNLOCK (self);

  encoded = soup_form_encode_datalist (&data);
  g_datalist_clear (&data);

  return encoded;
}

static gchar *
_get_file_name_from_uri (const gchar *uri, gboolean escaped)
{
  gchar *proto, *name = NULL;

  proto = gst_uri_get_protocol (uri);
  if (G_LIKELY (proto != NULL)) {
    gchar *filename = g_filename_from_uri (uri, NULL, NULL);

    if (filename) {
      gchar *basename = g_path_get_basename (filename);

      if (escaped) {
        name = g_uri_escape_string (basename, NULL, FALSE);
        g_free (basename);
      } else {
        name = basename;
      }

      g_free (filename);
    }
  }
  g_free (proto);

  return name;
}

static gint64
_opensubtitles_search (ClapperSubber *self, SoupSession *session, JsonReader *guessit,
    const gchar *uri, const gchar *hash, GCancellable *cancellable, GError **error)
{
  SoupMessage *msg;
  SoupMessageHeaders *headers;
  GInputStream *stream;
  JsonReader *reader = NULL;
  gchar *req_uri, *query;
  gint64 file_id = 0;

  GST_DEBUG_OBJECT (self, "Search...");

  query = _generate_search_query (self, guessit, hash);
  GST_DEBUG_OBJECT (self, "Search request query:\n%s", query);

  req_uri = g_strdup_printf ("https://%s/api/v1/subtitles?%s", self->base_url, query);
  msg = soup_message_new ("GET", req_uri);
  g_free (req_uri);

  headers = soup_message_get_request_headers (msg);
  soup_message_headers_append (headers, CLAPPER_SUBBER_UA_HEADER, CLAPPER_SUBBER_UA_STRING);
  soup_message_headers_append (headers, CLAPPER_SUBBER_LOGIN_HEADER, self->consumer);
  soup_message_headers_append (headers, CLAPPER_SUBBER_ACCEPT_HEADER, "application/json");

  stream = soup_session_send (session, msg, cancellable, error);
  g_object_unref (msg);

  if (stream) {
    reader = json_utils_read_stream (stream, cancellable, error);

    if (reader) {
      gchar *file_name;

      if (gst_debug_category_get_threshold (GST_CAT_DEFAULT) >= GST_LEVEL_DEBUG) {
        gchar *json = json_utils_reader_to_string (reader, TRUE);
        GST_DEBUG_OBJECT (self, "Search response:\n%s", json);
        g_free (json);
      }

      file_name = _get_file_name_from_uri (uri, FALSE);
      file_id = clapper_subber_ranker_choose_file_id (reader,
          file_name, cancellable, error);

      g_free (file_name);
      g_object_unref (reader);
    }

    g_object_unref (stream);
  }

  return file_id;
}

static JsonReader *
_opensubtitles_guessit (ClapperSubber *self, SoupSession *session, const gchar *uri,
    GCancellable *cancellable, GError **error)
{
  SoupMessage *msg;
  SoupMessageHeaders *headers;
  GInputStream *stream;
  JsonReader *reader = NULL;
  gchar *req_uri, *name;

  GST_DEBUG_OBJECT (self, "Guessit...");

  name = _get_file_name_from_uri (uri, TRUE);
  GST_DEBUG_OBJECT (self, "Escaped file name: \"%s\"", name);
  req_uri = g_strdup_printf ("https://%s/api/v1/utilities/guessit?filename=%s", self->base_url, name);
  g_free (name);

  msg = soup_message_new ("GET", req_uri);
  g_free (req_uri);

  headers = soup_message_get_request_headers (msg);
  soup_message_headers_append (headers, CLAPPER_SUBBER_UA_HEADER, CLAPPER_SUBBER_UA_STRING);
  soup_message_headers_append (headers, CLAPPER_SUBBER_LOGIN_HEADER, self->consumer);
  soup_message_headers_append (headers, CLAPPER_SUBBER_ACCEPT_HEADER, "application/json");

  stream = soup_session_send (session, msg, cancellable, error);
  g_object_unref (msg);

  if (stream) {
    reader = json_utils_read_stream (stream, cancellable, error);

    if (reader && gst_debug_category_get_threshold (GST_CAT_DEFAULT) >= GST_LEVEL_DEBUG) {
      gchar *json = json_utils_reader_to_string (reader, TRUE);
      GST_DEBUG_OBJECT (self, "Guessit response:\n%s", json);
      g_free (json);
    }

    g_object_unref (stream);
  }

  return reader;
}

static gboolean
_opensubtitles_login (ClapperSubber *self, SoupSession *session,
    GCancellable *cancellable, GError **error)
{
  SoupMessage *msg;
  SoupMessageHeaders *headers;
  GBytes *bytes;
  GInputStream *stream;
  gchar *req_body;

  /* Clear old values */
  g_clear_pointer (&self->base_url, g_free);
  g_clear_pointer (&self->token, g_free);

  msg = soup_message_new ("POST", "https://api.opensubtitles.com/api/v1/login");

  JSON_UTILS_BUILD_OBJECT (&req_body, {
    JSON_UTILS_ADD_KEY_VAL_STRING ("username", self->username);
    JSON_UTILS_ADD_KEY_VAL_STRING ("password", self->password);
  });

  bytes = g_bytes_new_take (req_body, strlen (req_body));
  soup_message_set_request_body_from_bytes (msg, "application/json", bytes);
  g_bytes_unref (bytes);

  headers = soup_message_get_request_headers (msg);
  soup_message_headers_append (headers, CLAPPER_SUBBER_UA_HEADER, CLAPPER_SUBBER_UA_STRING);
  soup_message_headers_append (headers, CLAPPER_SUBBER_LOGIN_HEADER, self->consumer);
  soup_message_headers_append (headers, CLAPPER_SUBBER_ACCEPT_HEADER, "application/json");

  stream = soup_session_send (session, msg, cancellable, error);
  g_object_unref (msg);

  if (stream) {
    JsonReader *reader;

    if ((reader = json_utils_read_stream (stream, cancellable, error))) {
      if (gst_debug_category_get_threshold (GST_CAT_DEFAULT) >= GST_LEVEL_DEBUG) {
        gchar *json = json_utils_reader_to_string (reader, TRUE);
        GST_DEBUG_OBJECT (self, "Login response:\n%s", json);
        g_free (json);
      }

      if (json_utils_get_int (reader, "status", NULL) == 200) {
        self->base_url = g_strdup (json_utils_get_string (reader, "base_url", NULL));
        self->token = g_strdup (json_utils_get_string (reader, "token", NULL));
      }
      if (self->base_url && self->token) {
        GST_DEBUG_OBJECT (self, "Base URL: \"%s\", token: %s",
            GST_STR_NULL (self->base_url), GST_STR_NULL (self->token));
      } else {
        g_set_error (error, G_IO_ERROR, G_IO_ERROR_FAILED,
            "Could not obtain OpenSubtitles %s",
            (!self->token) ? "token" : "base_url");
        g_clear_pointer (&self->base_url, g_free);
        g_clear_pointer (&self->token, g_free);
      }

      g_object_unref (reader);
    }

    g_object_unref (stream);
  }

  return (self->base_url && self->token);
}

static void
_opensubtitles_logout (ClapperSubber *self, SoupSession *session,
    GCancellable *cancellable, GError **error)
{
  SoupMessage *msg;
  SoupMessageHeaders *headers;
  GInputStream *stream;
  gchar *req_uri, *auth;

  GST_DEBUG_OBJECT (self, "Logout...");

  req_uri = g_strdup_printf ("https://%s/api/v1/logout", self->base_url);
  msg = soup_message_new ("DELETE", req_uri);
  g_free (req_uri);

  auth = g_strdup_printf ("Bearer %s", self->token);

  headers = soup_message_get_request_headers (msg);
  soup_message_headers_append (headers, CLAPPER_SUBBER_UA_HEADER, CLAPPER_SUBBER_UA_STRING);
  soup_message_headers_append (headers, CLAPPER_SUBBER_LOGIN_HEADER, self->consumer);
  soup_message_headers_append (headers, CLAPPER_SUBBER_AUTH_HEADER, auth);
  soup_message_headers_append (headers, CLAPPER_SUBBER_ACCEPT_HEADER, "application/json");

  stream = soup_session_send (session, msg, cancellable, error);
  g_object_unref (msg);

  if (stream && gst_debug_category_get_threshold (GST_CAT_DEFAULT) >= GST_LEVEL_DEBUG) {
    JsonReader *reader;

    if ((reader = json_utils_read_stream (stream, NULL, NULL))) {
      gchar *json = json_utils_reader_to_string (reader, TRUE);

      GST_DEBUG_OBJECT (self, "Logout response:\n%s", json);

      g_free (json);
      g_object_unref (reader);
    }
  }

  g_clear_object (&stream);

  /* Ensure logout is done only once */
  self->logged_in = FALSE;
  g_clear_pointer (&self->token, g_free);
  g_clear_pointer (&self->base_url, g_free);
}

static gchar *
_opensubtitles_do_workflow (ClapperSubber *self, const gchar *uri,
    const gchar *hash, GCancellable *cancellable, GError **error)
{
  SoupSession *session = soup_session_new_with_options ("timeout", 5, NULL);
  GFile *dl_file = NULL;
  gchar *suburi = NULL;
  gint64 file_id = 0;
  gboolean login_done = FALSE, dl_failed = FALSE;

login:
  GST_OBJECT_LOCK (self);
  if (!self->logged_in) {
    self->logged_in = _opensubtitles_login (self, session, cancellable, error);
    login_done = TRUE;
  }
  GST_OBJECT_UNLOCK (self);

  if (*error == NULL && file_id == 0) {
    JsonReader *guessit;

    if ((guessit = _opensubtitles_guessit (self, session, uri, cancellable, error))) {
      file_id = _opensubtitles_search (self, session, guessit, uri, hash, cancellable, error);
      g_object_unref (guessit);
    }
  }

  /* On the second try file ID can be present, so skip
   * above search logic and just retry download */
  if (*error == NULL && file_id > 0) {
    dl_file = _opensubtitles_download (self, session, file_id, cancellable, error);
    dl_failed = (dl_file == NULL);
  }

  /* When we did not login this time, but got download error (other than cancelled),
   * lets login again - possibly token expired if app was kept open for a long time. */
  if (self->logged_in && dl_failed && !login_done
      && !g_cancellable_is_cancelled (cancellable)) {
    GST_INFO_OBJECT (self, "First download try failed, reason: %s",
        (*error) ? (*error)->message : "Unknown");
    g_clear_error (error);

    /* Delete old token, and try to fetch a new one */
    _opensubtitles_logout (self, cancellable, error);
    if (*error == NULL && !g_cancellable_is_cancelled (cancellable))
      goto login;
  }

  if (dl_file) {
    suburi = g_file_get_uri (dl_file);
    g_object_unref (dl_file);
  }

  g_object_unref (session);

  return suburi;
}

static void
_download_in_thread (GTask *task, ClapperSubber *self,
    ClapperMediaItem *item, GCancellable *cancellable)
{
  GError *error = NULL;
  const gchar *uri;
  gchar *redirect_uri, *suburi = NULL;

  /* Prefer redirect as URI for hash generation */
  redirect_uri = clapper_media_item_get_redirect_uri (item);
  uri = (redirect_uri) ? redirect_uri : clapper_media_item_get_uri (item);

  if (_is_supported_proto (uri)) {
    gchar *hash;

    /* Generate file data hash as preferred by OpenSubtitles, so we can
     * improve our matched result. File hash can be NULL here without
     * error when data is not seekable */
    hash = _generate_data_hash (self, uri, cancellable, &error);
    if (!error) {
      if (hash)
        GST_DEBUG_OBJECT (self, "Generated file hash: %s", hash);
      else
        GST_WARNING_OBJECT (self, "Trying to find subtitles without data hash");

      suburi = _opensubtitles_do_workflow (self, uri, hash, cancellable, &error);
      g_free (hash);
    }
  }

  g_free (redirect_uri);

  if (suburi)
    g_task_return_pointer (task, suburi, g_free);
  else
    g_task_return_error (task, error);
}

static void
_download_async_cb (ClapperSubber *self, GAsyncResult *res, gpointer user_data G_GNUC_UNUSED)
{
  GTask *task = G_TASK (res);
  ClapperMediaItem *item;
  GError *error = NULL;
  gchar *suburi;

  item = CLAPPER_MEDIA_ITEM_CAST (g_task_get_task_data (task));
  suburi = (gchar *) g_task_propagate_pointer (task, &error);

  if (!error) {
    GST_DEBUG_OBJECT (self, "Setting subtitles URI: \"%s\"", suburi);
    clapper_media_item_set_suburi (item, suburi);
    g_free (suburi);
  } else {
    GST_ERROR_OBJECT (self, "OpenSubtitles error: %s", error->message);
    g_error_free (error);
  }

  GST_OBJECT_LOCK (self);
  if (G_UNLIKELY (!g_ptr_array_remove (self->active_downloads, item))) {
    GST_ERROR_OBJECT (self, "Could not remove %" GST_PTR_FORMAT
        " from active downloads list"); // Should never happen
  }
  GST_OBJECT_UNLOCK (self);
}

static void
clapper_subber_opensubtitles_download_async (ClapperSubber *self, ClapperMediaItem *item)
{
  gboolean in_progress;

  _ensure_consumer (self);

  GST_OBJECT_LOCK (self);
  if (!(in_progress = g_ptr_array_find (self->active_downloads, item, NULL)))
    g_ptr_array_add (self->active_downloads, gst_object_ref (item));
  GST_OBJECT_UNLOCK (self);

  if (!in_progress) {
    GTask *task = g_task_new (self, self->cancellable,
        (GAsyncReadyCallback) _download_async_cb, NULL);
    g_task_set_task_data (task, gst_object_ref (item), gst_object_unref);
    g_task_run_in_thread (task, (GTaskThreadFunc) _download_in_thread);
    g_object_unref (task);
  } else {
    GST_INFO_OBJECT (self, "Subtitles download for %" GST_PTR_FORMAT
        " is already in progress", item);
  }
}

static void
clapper_subber_played_item_changed (ClapperReactable *reactable, ClapperMediaItem *item)
{
  ClapperSubber *self = CLAPPER_SUBBER_CAST (reactable);

  GST_DEBUG_OBJECT (self, "Played item changed to: %" GST_PTR_FORMAT, item);
  gst_object_replace ((GstObject **) &self->played_item, GST_OBJECT_CAST (item));

  if (self->played_item)
    clapper_subber_opensubtitles_download_async (self, self->played_item);
}

static void
clapper_subber_queue_cleared (ClapperReactable *reactable)
{
  ClapperSubber *self = CLAPPER_SUBBER_CAST (reactable);

  gst_clear_object (&self->played_item);
}

static void
clapper_subber_reactable_iface_init (ClapperReactableInterface *iface)
{
  iface->played_item_changed = clapper_subber_played_item_changed;
  iface->queue_cleared = clapper_subber_queue_cleared;
}

#define parent_class clapper_subber_parent_class
G_DEFINE_TYPE_WITH_CODE (ClapperSubber, clapper_subber, GST_TYPE_OBJECT,
    G_IMPLEMENT_INTERFACE (CLAPPER_TYPE_REACTABLE, clapper_subber_reactable_iface_init));

static void
clapper_subber_init (ClapperSubber *self)
{
  self->active_downloads = g_ptr_array_new_with_free_func ((GDestroyNotify) gst_object_unref);
}

static void
clapper_subber_dispose (GObject *object)
{
  ClapperSubber *self = CLAPPER_SUBBER_CAST (object);

  if (self->cancellable) {
    g_cancellable_cancel (self->cancellable);
    g_clear_object (&self->cancellable);
  }

  if (self->logged_in)
    _opensubtitles_logout (self, NULL, NULL);

  g_ptr_array_unref (self->active_downloads);
  gst_clear_object (&self->played_item);

  G_OBJECT_CLASS (parent_class)->dispose (object);
}

static void
clapper_subber_finalize (GObject *object)
{
  ClapperSubber *self = CLAPPER_SUBBER_CAST (object);

  GST_TRACE_OBJECT (self, "Finalize");

  g_free (self->consumer);

  g_free (self->username);
  g_free (self->password);
  g_free (self->langs_order);

  G_OBJECT_CLASS (parent_class)->finalize (object);
}

static void
clapper_subber_set_property (GObject *object, guint prop_id,
    const GValue *value, GParamSpec *pspec)
{
  ClapperSubber *self = CLAPPER_SUBBER_CAST (object);

  switch (prop_id) {
    case PROP_OPENSUBTITLES_USERNAME:
      g_free (self->username);
      self->username = g_value_dup_string (value);
      break;
    case PROP_OPENSUBTITLES_PASSWORD:
      g_free (self->password);
      self->password = g_value_dup_string (value);
      break;
    case PROP_LANGUAGES_ORDER:
      g_free (self->langs_order);
      self->langs_order = g_value_dup_string (value);
      break;
    case PROP_AI_TRANSLATED:
      self->ai_translated = g_value_get_enum (value);
      break;
    case PROP_HEARING_IMPAIRED:
      self->hearing_impaired = g_value_get_enum (value);
      break;
    case PROP_MACHINE_TRANSLATED:
      self->machine_translated = g_value_get_enum (value);
      break;
    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
      break;
  }
}

static void
clapper_subber_get_property (GObject *object, guint prop_id,
    GValue *value, GParamSpec *pspec)
{
  ClapperSubber *self = CLAPPER_SUBBER_CAST (object);

  switch (prop_id) {
    case PROP_OPENSUBTITLES_USERNAME:
      g_value_set_string (value, self->username);
      break;
    case PROP_OPENSUBTITLES_PASSWORD:
      g_value_set_string (value, self->password);
      break;
    case PROP_LANGUAGES_ORDER:
      g_value_set_string (value, self->langs_order);
      break;
    case PROP_AI_TRANSLATED:
      g_value_set_enum (value, self->ai_translated);
      break;
    case PROP_HEARING_IMPAIRED:
      g_value_set_enum (value, self->hearing_impaired);
      break;
    case PROP_MACHINE_TRANSLATED:
      g_value_set_enum (value, self->machine_translated);
      break;
    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
      break;
  }
}

static void
clapper_subber_class_init (ClapperSubberClass *klass)
{
  GObjectClass *gobject_class = (GObjectClass *) klass;

  GST_DEBUG_CATEGORY_INIT (GST_CAT_DEFAULT, "clappersubber", 0,
      "Clapper Subber");
  clapper_subber_ranker_debug_init ();

  gobject_class->get_property = clapper_subber_get_property;
  gobject_class->set_property = clapper_subber_set_property;
  gobject_class->dispose = clapper_subber_dispose;
  gobject_class->finalize = clapper_subber_finalize;

  /**
   * ClapperSubber:opensubtitles-username:
   *
   * Username for OpenSubtitles account
   */
  param_specs[PROP_OPENSUBTITLES_USERNAME] = g_param_spec_string ("opensubtitles-username",
      "OpenSubtitles Username", "Username for OpenSubtitles account", NULL,
      G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS | CLAPPER_ENHANCER_PARAM_GLOBAL);

  /**
   * ClapperSubber:opensubtitles-password:
   *
   * Password for OpenSubtitles account
   */
  param_specs[PROP_OPENSUBTITLES_PASSWORD] = g_param_spec_string ("opensubtitles-password",
      "OpenSubtitles Password", "Password for OpenSubtitles account", NULL,
      G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS | CLAPPER_ENHANCER_PARAM_GLOBAL);

  /**
   * ClapperSubber:languages-order:
   *
   * Comma-separated order of preferred language ISO‑639‑1 codes
   */
  param_specs[PROP_LANGUAGES_ORDER] = g_param_spec_string ("languages-order",
      "Languages Order", "Comma-separated order of preferred language ISO‑639‑1 codes", NULL,
      G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS | CLAPPER_ENHANCER_PARAM_GLOBAL);

  /**
   * ClapperSubber:ai-translated:
   *
   * Whether to include AI translated subtitles
   */
  param_specs[PROP_AI_TRANSLATED] = g_param_spec_enum ("ai-translated",
      "AI Translated", "Whether to include AI translated subtitles",
      CLAPPER_SUBBER_TYPE_INCLUSION_SIMPLE, DEFAULT_AI_TRANSLATED,
      G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS | CLAPPER_ENHANCER_PARAM_GLOBAL);

  /**
   * ClapperSubber:hearing-impaired:
   *
   * Whether to include hearing impaired subtitles
   */
  param_specs[PROP_HEARING_IMPAIRED] = g_param_spec_enum ("hearing-impaired",
      "Hearing Impaired", "Whether to include hearing impaired subtitles",
      CLAPPER_SUBBER_TYPE_INCLUSION, DEFAULT_HEARING_IMPAIRED,
      G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS | CLAPPER_ENHANCER_PARAM_GLOBAL);

  /**
   * ClapperSubber:machine-translated:
   *
   * Whether to include machine translated subtitles
   */
  param_specs[PROP_MACHINE_TRANSLATED] = g_param_spec_enum ("machine-translated",
      "Machine Translated", "Whether to include machine translated subtitles",
      CLAPPER_SUBBER_TYPE_INCLUSION_SIMPLE, DEFAULT_MACHINE_TRANSLATED,
      G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS | CLAPPER_ENHANCER_PARAM_GLOBAL);

  g_object_class_install_properties (gobject_class, PROP_LAST, param_specs);
}

void
peas_register_types (PeasObjectModule *module)
{
  peas_object_module_register_extension_type (module, CLAPPER_TYPE_REACTABLE, CLAPPER_TYPE_SUBBER);
}
