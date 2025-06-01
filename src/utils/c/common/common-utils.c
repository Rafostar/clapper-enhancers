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

#include "common-utils.h"

gchar *
common_utils_match_regex (const gchar *expression, const gchar *input)
{
  GRegex *regex;
  GMatchInfo *match_info;
  gchar *str = NULL;

  regex = g_regex_new (expression, 0, 0, NULL);

  if (g_regex_match (regex, input, 0, &match_info))
    str = g_match_info_fetch (match_info, 1);

  g_match_info_free (match_info);
  g_regex_unref (regex);

  return str;
}
