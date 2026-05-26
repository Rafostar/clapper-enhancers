/* Clapper Enhancer Dir Walker
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

#include <glib.h>
#include <glib-object.h>
#include <gio/gio.h>
#include <gmodule.h>
#include <libpeas.h>
#include <clapper/clapper.h>

#define DEFAULT_RECURSIVE FALSE

#define GST_CAT_DEFAULT clapper_dir_walker_debug
GST_DEBUG_CATEGORY_STATIC (GST_CAT_DEFAULT);

#define CLAPPER_TYPE_DIR_WALKER (clapper_dir_walker_get_type())
#define CLAPPER_DIR_WALKER_CAST(obj) ((ClapperDirWalker *)(obj))
G_DECLARE_FINAL_TYPE (ClapperDirWalker, clapper_dir_walker, CLAPPER, DIR_WALKER, GstObject);

G_MODULE_EXPORT void peas_register_types (PeasObjectModule *module);

struct _ClapperDirWalker
{
  GstObject parent;

  gboolean recursive;
};

enum
{
  PROP_0,
  PROP_RECURSIVE,
  PROP_LAST
};

static GParamSpec *param_specs[PROP_LAST] = { NULL, };

static const gchar *playlist_types[] = {
  "audio/x-mpegurl",
  "audio/x-scpls"
};

static gboolean
_is_playlist_type (const gchar *content_type)
{
  guint i;

  for (i = 0; i < G_N_ELEMENTS (playlist_types); ++i) {
    if (strcmp (content_type, playlist_types[i]) == 0)
      return TRUE;
  }
  return FALSE;
}

static gboolean
_is_media_file (GFile *file, GCancellable *cancellable, GError **error)
{
  GFileInfo *info;
  gboolean is_media = FALSE;

  if ((info = g_file_query_info (file,
      G_FILE_ATTRIBUTE_STANDARD_CONTENT_TYPE ","
      G_FILE_ATTRIBUTE_STANDARD_FAST_CONTENT_TYPE,
      G_FILE_QUERY_INFO_NOFOLLOW_SYMLINKS,
      cancellable, error))) {
    const gchar *content_type = NULL;

    if (g_file_info_has_attribute (info,
        G_FILE_ATTRIBUTE_STANDARD_CONTENT_TYPE)) {
      content_type = g_file_info_get_content_type (info);
    } else if (g_file_info_has_attribute (info,
        G_FILE_ATTRIBUTE_STANDARD_FAST_CONTENT_TYPE)) {
      content_type = g_file_info_get_attribute_string (info,
          G_FILE_ATTRIBUTE_STANDARD_FAST_CONTENT_TYPE);
    }

    if (content_type) {
      /* NOTE: Playlist files are not included, as they likely contain
       * media from the same dir, so these files would be added twice. */
      is_media = (g_str_has_prefix (content_type, "video/")
          || (g_str_has_prefix (content_type, "audio/")
          && !_is_playlist_type (content_type)));
    }

    g_object_unref (info);
  }

  return is_media;
}

static gboolean
_walk_dir (ClapperDirWalker *self, GFile *dir, GPtrArray *uris,
    GCancellable *cancellable, GError **error)
{
  GFileEnumerator *dir_enum;

  if (!(dir_enum = g_file_enumerate_children (dir,
      G_FILE_ATTRIBUTE_STANDARD_NAME ","
      G_FILE_ATTRIBUTE_STANDARD_TYPE,
      G_FILE_QUERY_INFO_NOFOLLOW_SYMLINKS,
      cancellable, error)))
    return FALSE;

  while (TRUE) {
    GFileInfo *info = NULL;
    GFile *child = NULL;

    if (!g_file_enumerator_iterate (dir_enum, &info,
        &child, cancellable, error) || !info)
      break;

    if (g_file_info_get_file_type (info) == G_FILE_TYPE_DIRECTORY) {
      if (self->recursive && !_walk_dir (self, child, uris, cancellable, error))
        break;
    } else if (_is_media_file (child, cancellable, error)) {
      g_ptr_array_add (uris, g_file_get_uri (child));
    }
  }

  g_object_unref (dir_enum);

  return (*error == NULL);
}

static inline gchar *
_generate_manifest (GPtrArray *uris)
{
  GString *string = g_string_new (NULL);
  guint i;

  for (i = 0; i < uris->len; ++i) {
    g_string_append (string, g_ptr_array_index (uris, i));
    g_string_append_c (string, '\n');
  }

  return g_string_free (string, FALSE);
}

static gboolean
clapper_dir_walker_extract (ClapperExtractable *extractable, GUri *uri, ClapperHarvest *harvest,
    GCancellable *cancellable, GError **error)
{
  ClapperDirWalker *self = CLAPPER_DIR_WALKER_CAST (extractable);
  GFile *file;
  gchar *uri_str;
  gboolean success = FALSE;

  GST_DEBUG_OBJECT (self, "Extract");

  uri_str = g_uri_to_string (uri);
  file = g_file_new_for_uri (uri_str);

  /* If extracted file is a directory, walk through its contents. Otherwise forward URI as-is. */
  if (g_file_query_file_type (file, G_FILE_QUERY_INFO_NONE, cancellable) == G_FILE_TYPE_DIRECTORY) {
    GPtrArray *uris = g_ptr_array_new_with_free_func (g_free);

    if (_walk_dir (self, file, uris, cancellable, error)) {
      gchar *manifest;

      /* Sort all file URIs alphabetically */
      if (uris->len > 1)
        g_ptr_array_sort_values (uris, (GCompareFunc) strcmp);

      if ((manifest = _generate_manifest (uris)))
        success = clapper_harvest_fill_with_text (harvest, "text/uri-list", manifest);
    }

    g_ptr_array_unref (uris);
    g_free (uri_str);
  } else if (!g_cancellable_is_cancelled (cancellable)) {
    /* Every other type (including "UNKNOWN") should be forwarded
     * to let player try to play it (unless extraction was cancelled). */
    success = clapper_harvest_fill_with_text (harvest, "text/x-uri", uri_str); // transfer full
  }

  g_object_unref (file);

  GST_DEBUG_OBJECT (self, "Extraction %s", (success) ? "succeded" : "failed");

  return success;
}

static void
clapper_dir_walker_extractable_iface_init (ClapperExtractableInterface *iface)
{
  iface->extract = clapper_dir_walker_extract;
}

#define parent_class clapper_dir_walker_parent_class
G_DEFINE_TYPE_WITH_CODE (ClapperDirWalker, clapper_dir_walker, GST_TYPE_OBJECT,
    G_IMPLEMENT_INTERFACE (CLAPPER_TYPE_EXTRACTABLE, clapper_dir_walker_extractable_iface_init));

static void
clapper_dir_walker_init (ClapperDirWalker *self)
{
  self->recursive = DEFAULT_RECURSIVE;
}

static void
clapper_dir_walker_finalize (GObject *object)
{
  ClapperDirWalker *self = CLAPPER_DIR_WALKER_CAST (object);

  GST_TRACE_OBJECT (self, "Finalize");

  G_OBJECT_CLASS (parent_class)->finalize (object);
}

static void
clapper_dir_walker_set_property (GObject *object, guint prop_id,
    const GValue *value, GParamSpec *pspec)
{
  ClapperDirWalker *self = CLAPPER_DIR_WALKER_CAST (object);

  switch (prop_id) {
    case PROP_RECURSIVE:
      self->recursive = g_value_get_boolean (value);
      break;
    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
      break;
  }
}

static void
clapper_dir_walker_get_property (GObject *object, guint prop_id,
    GValue *value, GParamSpec *pspec)
{
  ClapperDirWalker *self = CLAPPER_DIR_WALKER_CAST (object);

  switch (prop_id) {
    case PROP_RECURSIVE:
      g_value_set_boolean (value, self->recursive);
      break;
    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
      break;
  }
}

static void
clapper_dir_walker_class_init (ClapperDirWalkerClass *klass)
{
  GObjectClass *gobject_class = (GObjectClass *) klass;

  GST_DEBUG_CATEGORY_INIT (GST_CAT_DEFAULT, "clapperdirwalker", 0,
      "Clapper Dir Walker");

  gobject_class->get_property = clapper_dir_walker_get_property;
  gobject_class->set_property = clapper_dir_walker_set_property;
  gobject_class->finalize = clapper_dir_walker_finalize;

  /**
   * ClapperDirWalker:recursive:
   *
   * Walk through subdirectiores.
   */
  param_specs[PROP_RECURSIVE] = g_param_spec_boolean ("recursive",
      "Recursive", "Walk through subdirectiores", DEFAULT_RECURSIVE,
      G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS | CLAPPER_ENHANCER_PARAM_GLOBAL);

  g_object_class_install_properties (gobject_class, PROP_LAST, param_specs);
}

void
peas_register_types (PeasObjectModule *module)
{
  peas_object_module_register_extension_type (module, CLAPPER_TYPE_EXTRACTABLE, CLAPPER_TYPE_DIR_WALKER);
}
