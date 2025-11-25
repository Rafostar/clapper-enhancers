/* Clapper Enhancer Parser M3U
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

#include <glib.h>
#include <glib-object.h>
#include <gio/gio.h>
#include <gmodule.h>
#include <libpeas.h>
#include <clapper/clapper.h>

#include <gst/gst.h>
#include <gst/tag/tag.h>

#define GST_CAT_DEFAULT clapper_parser_m3u_debug
GST_DEBUG_CATEGORY_STATIC (GST_CAT_DEFAULT);

#define CLAPPER_TYPE_PARSER_M3U (clapper_parser_m3u_get_type())
#define CLAPPER_PARSER_M3U_CAST(obj) ((ClapperParserM3u *)(obj))
G_DECLARE_FINAL_TYPE (ClapperParserM3u, clapper_parser_m3u, CLAPPER, PARSER_M3U, GstObject);

G_MODULE_EXPORT void peas_register_types (PeasObjectModule *module);

struct _ClapperParserM3u
{
  GstObject parent;
};

static GstTagList *
_parse_extinf_data (ClapperParserM3u *self, const gchar *ptr, gsize len)
{
  GstTagList *tags = NULL;
  gchar *line, *dur_end = NULL;
  gdouble duration;

  line = g_strndup (ptr, len);
  GST_DEBUG_OBJECT (self, "Parsing line: %s", line);

  /* Read after length of "#EXTINF:" */
  duration = g_ascii_strtod (line + 8, &dur_end);

  if (duration > 0) {
    GST_DEBUG_OBJECT (self, "Found duration: %" CLAPPER_TIME_FORMAT,
        CLAPPER_TIME_ARGS (duration));
    tags = gst_tag_list_new (
        GST_TAG_DURATION, (guint64) (duration * GST_SECOND), NULL);
  }
  if (dur_end && *dur_end != '\0') {
    const gchar *title = dur_end + 1;

    if (*title != '\0') {
      GST_DEBUG_OBJECT (self, "Found title: %s", title);

      if (!tags)
        tags = gst_tag_list_new_empty ();

      gst_tag_list_add (tags, GST_TAG_MERGE_REPLACE,
          GST_TAG_TITLE, title, NULL);
    }
  }

  g_free (line);

  if (tags)
    gst_tag_list_set_scope (tags, GST_TAG_SCOPE_GLOBAL);

  return tags;
}

static ClapperMediaItem *
_parse_uri_data (ClapperParserM3u *self, GUri *uri, const gchar *ptr, gsize len, GError **error)
{
  ClapperMediaItem *item = NULL;
  gchar *line = g_strndup (ptr, len);

  GST_DEBUG_OBJECT (self, "Parsing line: %s", line);

  if (gst_uri_is_valid (line)) {
    GST_DEBUG_OBJECT (self, "Found URI: %s", line);
    item = clapper_media_item_new (line);
  } else {
    gchar *base_uri, *res_uri;

    base_uri = g_uri_to_string (uri);
    res_uri = g_uri_resolve_relative (base_uri, line, G_URI_FLAGS_ENCODED, error);
    g_free (base_uri);

    if (res_uri) {
      GST_DEBUG_OBJECT (self, "Resolved URI: %s", res_uri);
      item = clapper_media_item_new (res_uri);
      g_free (res_uri);
    }
  }

  g_free (line);

  return item;
}

static gboolean
clapper_parser_m3u_parse (ClapperPlaylistable *playlistable, GUri *uri, GBytes *bytes,
    GListStore *playlist, GCancellable *cancellable, GError **error)
{
  ClapperParserM3u *self = CLAPPER_PARSER_M3U_CAST (playlistable);
  GstTagList *tags = NULL;
  const gchar *ptr, *end;
  gsize data_size;
  gboolean success = FALSE;

  GST_DEBUG_OBJECT (self, "Parse");

  ptr = g_bytes_get_data (bytes, &data_size);
  end = ptr + data_size;

  while (ptr < end) {
    const gchar *nl = memchr (ptr, '\n', end - ptr);
    gsize len = nl ? nl - ptr : end - ptr;

    switch (ptr[0]) {
      case '#':
        if (g_str_has_prefix (ptr, "#EXTINF:"))
          gst_tag_list_replace (&tags, _parse_extinf_data (self, ptr, len));
        break;
      case '\0':
      case '\n':
      case ' ':
        break;
      default:{
        ClapperMediaItem *item;
        if ((item = _parse_uri_data (self, uri, ptr, len, error))) {
          if (tags) {
            clapper_media_item_populate_tags (item, tags);
            gst_clear_tag_list (&tags);
          }
          g_list_store_append (playlist, (GObject *) item);
          gst_object_unref (item);
          success = TRUE;
        }
        break;
      }
    }

    if (G_UNLIKELY (*error != NULL) || g_cancellable_is_cancelled (cancellable)) {
      success = FALSE;
      break;
    }

    /* Advance to the next line */
    ptr = nl ? (nl + 1) : end;
  }

  gst_clear_tag_list (&tags);

  GST_DEBUG_OBJECT (self, "Parsing %s", (success)
      ? "succeeded"
      : g_cancellable_is_cancelled (cancellable)
      ? "cancelled"
      : "failed");

  return success;
}

static void
clapper_parser_m3u_playlistable_iface_init (ClapperPlaylistableInterface *iface)
{
  iface->parse = clapper_parser_m3u_parse;
}

#define parent_class clapper_parser_m3u_parent_class
G_DEFINE_TYPE_WITH_CODE (ClapperParserM3u, clapper_parser_m3u, GST_TYPE_OBJECT,
    G_IMPLEMENT_INTERFACE (CLAPPER_TYPE_PLAYLISTABLE, clapper_parser_m3u_playlistable_iface_init));

static void
clapper_parser_m3u_init (ClapperParserM3u *self)
{
}

static void
clapper_parser_m3u_finalize (GObject *object)
{
  GST_TRACE_OBJECT (object, "Finalize");

  G_OBJECT_CLASS (parent_class)->finalize (object);
}

static void
clapper_parser_m3u_class_init (ClapperParserM3uClass *klass)
{
  GObjectClass *gobject_class = (GObjectClass *) klass;

  GST_DEBUG_CATEGORY_INIT (GST_CAT_DEFAULT, "clapperparserm3u", 0,
      "Clapper Parser M3U");

  gobject_class->finalize = clapper_parser_m3u_finalize;
}

void
peas_register_types (PeasObjectModule *module)
{
  peas_object_module_register_extension_type (module, CLAPPER_TYPE_PLAYLISTABLE, CLAPPER_TYPE_PARSER_M3U);
}
