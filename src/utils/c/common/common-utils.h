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
 * License along with this library; if not, see
 * <https://www.gnu.org/licenses/>.
 */

#pragma once

#include <glib.h>

#define COMMON_UTILS_DEFINE_ENUM_TYPE(TypeName, type_name, ...)  \
    GType type_name_##_get_type (void) {                         \
      static gsize gtype_id = 0;                                 \
      if (g_once_init_enter (&gtype_id)) {                       \
        GType new_type = g_type_from_name (#TypeName);           \
        if (new_type == 0) {                                     \
          static const GEnumValue values[] = {                   \
            __VA_ARGS__,                                         \
            { 0, NULL, NULL }                                    \
          };                                                     \
          new_type = g_enum_register_static (                    \
              g_intern_static_string (#TypeName), values);       \
        }                                                        \
        g_once_init_leave (&gtype_id, new_type);                 \
    }                                                            \
    return (GType) gtype_id; }

#define COMMON_UTILS_DEFINE_FLAGS_TYPE(TypeName, type_name, ...) \
    GType type_name_##_get_type (void) {                         \
      static gsize gtype_id = 0;                                 \
      if (g_once_init_enter (&gtype_id)) {                       \
        GType new_type = g_type_from_name (#TypeName);           \
        if (new_type == 0) {                                     \
          static const GFlagsValue values[] = {                  \
            __VA_ARGS__,                                         \
            { 0, NULL, NULL }                                    \
          };                                                     \
          new_type = g_flags_register_static (                   \
              g_intern_static_string (#TypeName), values);       \
        }                                                        \
        g_once_init_leave (&gtype_id, new_type);                 \
    }                                                            \
    return (GType) gtype_id; }

G_BEGIN_DECLS

gchar * common_utils_match_regex (const gchar *expression, const gchar *input);

G_END_DECLS
