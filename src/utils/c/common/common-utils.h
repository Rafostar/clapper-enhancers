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
#include <glib-object.h>

#if GLIB_CHECK_VERSION(2, 80, 0)
#define _once_init_type GType
#define _once_init_enter g_once_init_enter_pointer
#define _once_init_leave g_once_init_leave_pointer
#else
#define _once_init_type gsize
#define _once_init_enter g_once_init_enter
#define _once_init_leave g_once_init_leave
#endif

#define COMMON_UTILS_DEFINE_ENUM_TYPE(TypeName, type_name, ...)  \
    GType type_name##_get_type (void) {                          \
      static _once_init_type gtype_id = 0;                       \
      if (_once_init_enter (&gtype_id)) {                        \
        GType new_type = g_type_from_name (#TypeName);           \
        if (new_type == 0) {                                     \
          static const GEnumValue values[] = {                   \
            __VA_ARGS__,                                         \
            { 0, NULL, NULL }                                    \
          };                                                     \
          new_type = g_enum_register_static (                    \
              g_intern_static_string (#TypeName), values);       \
        }                                                        \
        _once_init_leave (&gtype_id, new_type);                  \
    }                                                            \
    return gtype_id; }

#define COMMON_UTILS_DEFINE_FLAGS_TYPE(TypeName, type_name, ...) \
    GType type_name##_get_type (void) {                          \
      static _once_init_type gtype_id = 0;                       \
      if (_once_init_enter (&gtype_id)) {                        \
        GType new_type = g_type_from_name (#TypeName);           \
        if (new_type == 0) {                                     \
          static const GFlagsValue values[] = {                  \
            __VA_ARGS__,                                         \
            { 0, NULL, NULL }                                    \
          };                                                     \
          new_type = g_flags_register_static (                   \
              g_intern_static_string (#TypeName), values);       \
        }                                                        \
        _once_init_leave (&gtype_id, new_type);                  \
    }                                                            \
    return gtype_id; }

G_BEGIN_DECLS

gchar * common_utils_match_regex (const gchar *expression, const gchar *input);

G_END_DECLS
