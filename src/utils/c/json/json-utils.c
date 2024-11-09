/*
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

#include "json-utils.h"

#define JSON_UTILS_QUARK (_json_utils_get_quark ())

static inline GQuark
_json_utils_get_quark (void)
{
  return g_quark_from_static_string ("clapper-enhancers-json-utils-quark");
}

static gboolean
_json_reader_va_iter (JsonReader *reader, va_list args, guint *depth)
{
  gboolean success = TRUE;

  while (success) {
    gpointer arg;

    if (!(arg = va_arg (args, gpointer)))
      break;

    if ((success = json_reader_is_object (reader))) {
      const gchar *name = (const gchar *) arg;

      *depth += 1;
      if (!(success = json_reader_read_member (reader, name)))
        break;
    } else if ((success = json_reader_is_array (reader))) {
      guint index = GPOINTER_TO_UINT (arg);
      gint n_elems;

      /* Safety check */
      if (!(success = index > 0))
        break;

      index--;
      n_elems = json_reader_count_elements (reader);

      if (!(success = (n_elems > 0 && index < (guint) n_elems)))
        break;

      *depth += 1;
      if (!(success = json_reader_read_element (reader, index)))
        break;
    }
  }

  return success;
}

static JsonReader *
_make_reader_from_parser (JsonParser *parser)
{
  JsonReader *reader = json_reader_new (json_parser_get_root (parser));

  g_object_set_qdata_full ((GObject *) reader, JSON_UTILS_QUARK,
      g_object_ref (parser), (GDestroyNotify) g_object_unref);

  return reader;
}

static void
_ensure_parser_load_error (GError **error)
{
  if (G_UNLIKELY (error && *error == NULL)) {
    g_set_error (error, G_IO_ERROR, G_IO_ERROR_FAILED,
        "Could not load JSON data");
  }
}

JsonReader *
json_utils_read_stream (GInputStream *stream, GCancellable *cancellable, GError **error)
{
  JsonParser *parser = json_parser_new ();
  JsonReader *reader = NULL;

  if (json_parser_load_from_stream (parser, stream, cancellable, error))
    reader = _make_reader_from_parser (parser);
  else
    _ensure_parser_load_error (error);

  g_object_unref (parser);

  return reader;
}

JsonReader *
json_utils_read_data (const gchar *data, GError **error)
{
  JsonParser *parser = json_parser_new ();
  JsonReader *reader = NULL;

  if (json_parser_load_from_data (parser, data, -1, error))
    reader = _make_reader_from_parser (parser);
  else
    _ensure_parser_load_error (error);

  g_object_unref (parser);

  return reader;
}

const gchar *
json_utils_get_string (JsonReader *reader, ...)
{
  va_list args;
  const gchar *value = NULL;
  guint depth = 0;
  gboolean success;

  va_start (args, reader);
  success = _json_reader_va_iter (reader, args, &depth);
  va_end (args);

  /* Reading null as string makes reader stuck */
  if (success && json_reader_is_value (reader)
      && !json_reader_get_null_value (reader))
    value = json_reader_get_string_value (reader);

  json_utils_go_back (reader, depth);

  return value;
}

gint64
json_utils_get_int (JsonReader *reader, ...)
{
  va_list args;
  gint64 value = 0;
  guint depth = 0;
  gboolean success;

  va_start (args, reader);
  success = _json_reader_va_iter (reader, args, &depth);
  va_end (args);

  if (success && json_reader_is_value (reader))
    value = json_reader_get_int_value (reader);

  json_utils_go_back (reader, depth);

  return value;
}

gboolean
json_utils_get_boolean (JsonReader *reader, ...)
{
  va_list args;
  gboolean value = FALSE;
  guint depth = 0;
  gboolean success;

  va_start (args, reader);
  success = _json_reader_va_iter (reader, args, &depth);
  va_end (args);

  if (success && json_reader_is_value (reader))
    value = json_reader_get_boolean_value (reader);

  json_utils_go_back (reader, depth);

  return value;
}

gint
json_utils_count_elements (JsonReader *reader, ...)
{
  va_list args;
  gint n_elems = -1;
  guint depth = 0;
  gboolean success;

  va_start (args, reader);
  success = _json_reader_va_iter (reader, args, &depth);
  va_end (args);

  if (success && json_reader_is_array (reader))
    n_elems = json_reader_count_elements (reader);

  json_utils_go_back (reader, depth);

  return n_elems;
}

gboolean
json_utils_go_to (JsonReader *reader, ...)
{
  va_list args;
  guint depth = 0;
  gboolean success;

  va_start (args, reader);
  success = _json_reader_va_iter (reader, args, &depth);
  va_end (args);

  /* We do not go back here on success */
  if (!success)
    json_utils_go_back (reader, depth);

  return success;
}

void
json_utils_go_back (JsonReader *reader, guint count)
{
  /* FIXME: This is an abuse of json-glib mechanics, where
   * leaving array might position us in either parent array
   * or object thus result is the same as calling `end_member`
   * on an object. It would be nice if json-glib had an API
   * to check if we are currently inside of array */
  while (count--)
    json_reader_end_element (reader);
}

/*
 * json_utils_array_foreach:
 * @reader: a #JsonReader
 * @harvest: a #ClapperHarvest
 * @func: a #JsonUtilsForeachFunc to call for each array element
 * @user_data: user data to pass to the function
 *
 * Calls a function for each element of a array. Reader must
 * be at array each time callback function is called.
 *
 * Returns: %TRUE if array was found and iterated, %FALSE otherwise.
 */
gboolean
json_utils_array_foreach (JsonReader *reader, ClapperHarvest *harvest, JsonUtilsForeachFunc func, gpointer user_data)
{
  gint i, count;

  if (!json_reader_is_array (reader))
    return FALSE;

  count = json_reader_count_elements (reader);
  for (i = 0; i < count; i++) {
    gboolean cont = FALSE;

    if (json_reader_read_element (reader, i)) {
      cont = ((*func) (reader, harvest, user_data));
    }
    json_reader_end_element (reader);

    if (!cont)
      break;
  }

  return (count > 0);
}

static gchar *
_json_node_to_string_internal (JsonNode *node, gboolean pretty)
{
  JsonGenerator *gen;
  gchar *data;

  if (G_UNLIKELY (node == NULL))
    return NULL;

  gen = json_generator_new ();
  json_generator_set_pretty (gen, pretty);
  json_generator_set_root (gen, node);
  data = json_generator_to_data (gen, NULL);

  g_object_unref (gen);

  return data;
}

gchar *
json_utils_parser_to_string (JsonParser *parser, gboolean pretty)
{
  return _json_node_to_string_internal (json_parser_get_root (parser), pretty);
}

gchar *
json_utils_reader_to_string (JsonReader *reader, gboolean pretty)
{
  JsonNode *root = NULL;
  gchar *data = NULL;

  g_object_get (reader, "root", &root, NULL);

  if (root) {
    data = _json_node_to_string_internal (root, pretty);
    json_node_unref (root);
  }

  return data;
}
