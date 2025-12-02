/* Clapper Enhancer Control Hub
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

#pragma once

#include <glib.h>
#include <glib-object.h>
#include <clapper/clapper.h>
#include <microdns/microdns.h>

G_BEGIN_DECLS

#define CLAPPER_TYPE_CONTROL_HUB_MDNS (clapper_control_hub_mdns_get_type())
#define CLAPPER_CONTROL_HUB_MDNS_CAST(obj) ((ClapperControlHubMdns *)(obj))

G_GNUC_INTERNAL
G_DECLARE_FINAL_TYPE (ClapperControlHubMdns, clapper_control_hub_mdns, CLAPPER, CONTROL_HUB_MDNS, ClapperThreadedObject)

struct _ClapperControlHubMdns
{
  ClapperThreadedObject parent;

  struct mdns_ctx *ctx;
  gboolean initialized;

  gchar *domain_name;
  gchar *service_link;
  gchar **txt_records;

  /* Atomic */
  gint run;
  gint port;
};

G_GNUC_INTERNAL
ClapperControlHubMdns * clapper_control_hub_mdns_new (void);

G_GNUC_INTERNAL
void clapper_control_hub_mdns_start (ClapperControlHubMdns *mdns, gint port);

G_GNUC_INTERNAL
void clapper_control_hub_mdns_stop (ClapperControlHubMdns *mdns);

G_END_DECLS
