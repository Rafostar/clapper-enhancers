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

#pragma once

#include <glib.h>
#include <gio/gio.h>
#include <json-glib/json-glib.h>
#include <clapper/clapper.h>

G_BEGIN_DECLS

/*
 * Fills JSON data into gchar pointer, it is safe to call "break"
 * inside passed code block to cancel this operation.
 */
#define JSON_UTILS_BUILD_OBJECT(dest, ...) {                           \
    JsonBuilder *_utils_builder = json_builder_new ();                 \
    gboolean _obj_ok = FALSE;                                          \
    *dest = NULL;                                                      \
    while (TRUE) {                                                     \
      json_builder_begin_object (_utils_builder);                      \
      __VA_ARGS__                                                      \
      _obj_ok = (json_builder_end_object (_utils_builder) != NULL);    \
      break;                                                           \
    }                                                                  \
    if (_obj_ok) {                                                     \
      JsonGenerator *_utils_gen = json_generator_new ();               \
      JsonNode *_utils_root = json_builder_get_root (_utils_builder);  \
      json_generator_set_pretty (_utils_gen, TRUE);                    \
      json_generator_set_indent (_utils_gen, 2);                       \
      json_generator_set_root (_utils_gen, _utils_root);               \
      *dest = json_generator_to_data (_utils_gen, NULL);               \
      g_object_unref (_utils_gen);                                     \
      json_node_free (_utils_root);                                    \
    }                                                                  \
    g_object_unref (_utils_builder); }

#define JSON_UTILS_ADD_OBJECT(...)                                     \
    json_builder_begin_object (_utils_builder);                        \
    __VA_ARGS__                                                        \
    json_builder_end_object (_utils_builder);

#define JSON_UTILS_ADD_ARRAY(...)                                      \
    json_builder_begin_array (_utils_builder);                         \
    __VA_ARGS__                                                        \
    json_builder_end_array (_utils_builder);

#define JSON_UTILS_ADD_NAMED_OBJECT(name, ...)                         \
    json_builder_set_member_name (_utils_builder, name);               \
    json_builder_begin_object (_utils_builder);                        \
    __VA_ARGS__                                                        \
    json_builder_end_object (_utils_builder);

#define JSON_UTILS_ADD_NAMED_ARRAY(name, ...)                          \
    json_builder_set_member_name (_utils_builder, name);               \
    json_builder_begin_array (_utils_builder);                         \
    __VA_ARGS__                                                        \
    json_builder_end_array (_utils_builder);

#define JSON_UTILS_ADD_KEY_VAL_STRING(key, val)                        \
    json_builder_set_member_name (_utils_builder, key);                \
    json_builder_add_string_value (_utils_builder, val);

#define JSON_UTILS_ADD_KEY_VAL_YES_NO(key, val)                        \
    json_builder_set_member_name (_utils_builder, key);                \
    if (val)                                                           \
      json_builder_add_string_value (_utils_builder, "yes");           \
    else                                                               \
      json_builder_add_string_value (_utils_builder, "no");

#define JSON_UTILS_ADD_KEY_VAL_INT(key, val)                           \
    json_builder_set_member_name (_utils_builder, key);                \
    json_builder_add_int_value (_utils_builder, val);

#define JSON_UTILS_ADD_KEY_VAL_BOOLEAN(key, val)                       \
    json_builder_set_member_name (_utils_builder, key);                \
    json_builder_add_boolean_value (_utils_builder, val);

#define JSON_UTILS_ADD_VAL_STRING(val)                                 \
    json_builder_add_string_value (_utils_builder, val);

#define JSON_UTILS_ADD_VAL_INT(val)                                    \
    json_builder_add_int_value (_utils_builder, val);

#define JSON_UTILS_ADD_VAL_BOOLEAN(val)                                \
    json_builder_add_boolean_value (_utils_builder, val);

/* JSON array navigation, for use as VA arg */
#define JSON_UTILS_ARRAY_INDEX(index)                                  \
    GUINT_TO_POINTER (index + 1)

/* Return %TRUE to continue, %FALSE to stop iterating */
typedef gboolean (*JsonUtilsForeachFunc) (JsonReader *reader, ClapperHarvest *harvest, gpointer user_data);

JsonReader * json_utils_read_stream (GInputStream *stream, GCancellable *cancellable, GError **error);

JsonReader * json_utils_read_data (const gchar *data, GError **error);

const gchar * json_utils_get_string (JsonReader *reader, ...) G_GNUC_NULL_TERMINATED;

gint64 json_utils_get_int (JsonReader *reader, ...) G_GNUC_NULL_TERMINATED;

gboolean json_utils_get_boolean (JsonReader *reader, ...) G_GNUC_NULL_TERMINATED;

gint json_utils_count_elements (JsonReader *reader, ...) G_GNUC_NULL_TERMINATED;

gboolean json_utils_go_to (JsonReader *reader, ...) G_GNUC_NULL_TERMINATED;

void json_utils_go_back (JsonReader *reader, guint count);

gboolean json_utils_array_foreach (JsonReader *reader, ClapperHarvest *harvest, JsonUtilsForeachFunc func, gpointer user_data);

gchar * json_utils_parser_to_string (JsonParser *parser, gboolean pretty);

gchar * json_utils_reader_to_string (JsonReader *reader, gboolean pretty);

G_END_DECLS
