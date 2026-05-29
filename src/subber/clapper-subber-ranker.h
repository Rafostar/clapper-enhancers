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

#pragma once

#include <glib.h>
#include <gio/gio.h>
#include "../utils/c/json/json-utils.h"

G_BEGIN_DECLS

G_GNUC_INTERNAL
void clapper_subber_ranker_debug_init (void);

G_GNUC_INTERNAL
gint64 clapper_subber_ranker_choose_file_id (JsonReader *reader, const gchar *file_name, GCancellable *cancellable, GError **error);

G_END_DECLS
