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

#include "config.h"

#include <gst/gst.h>
#include <sys/socket.h>

#include "clapper-control-hub-mdns.h"

#define CLAPPER_CONTROL_HUB_MDNS_SERVICE "_clapper._tcp.local"

#define N_ANSWERS 4
#define N_TXT 4

#define PTR_INDEX 0
#define TXT_INDEX 1
#define SRV_INDEX 2
#define A_AAAA_INDEX 3

#define GST_CAT_DEFAULT clapper_control_hub_mdns_debug
GST_DEBUG_CATEGORY_STATIC (GST_CAT_DEFAULT);

#define parent_class clapper_control_hub_mdns_parent_class
G_DEFINE_TYPE (ClapperControlHubMdns, clapper_control_hub_mdns, CLAPPER_TYPE_THREADED_OBJECT);

static gint _cch_id = 0;

static inline void
_send_entries (ClapperControlHubMdns *self, const struct sockaddr *addr,
    enum mdns_announce_type type)
{
  gint i, port = g_atomic_int_get (&self->port);
  struct rr_entry answers[N_ANSWERS];
  struct mdns_hdr hdr = { 0, };
  struct rr_data_txt txt_data[N_TXT];

  GST_LOG_OBJECT (self, "Preparing answers for MDNS query, service: \"%s\""
      ", domain: \"%s\", link: \"%s\"",
      CLAPPER_CONTROL_HUB_MDNS_SERVICE, self->domain_name, self->service_link);

  hdr.flags |= FLAG_QR;
  hdr.flags |= FLAG_AA;
  hdr.num_ans_rr = N_ANSWERS;

  for (i = 0; i < N_TXT; ++i) {
    g_strlcpy (txt_data[i].txt, self->txt_records[i], sizeof (txt_data[i].txt));
    txt_data[i].next = (i < N_TXT - 1) ? &txt_data[i + 1] : NULL;
  }

  answers[PTR_INDEX] = (struct rr_entry) {
    .type = RR_PTR,
    .name = (char *) CLAPPER_CONTROL_HUB_MDNS_SERVICE,
    .data.PTR.domain = self->service_link,
    .rr_class = RR_IN,
    .msbit = 1,
    .ttl = (type == MDNS_ANNOUNCE_GOODBYE) ? 0 : 120,
    .next = &answers[TXT_INDEX]
  };

  answers[TXT_INDEX] = (struct rr_entry) {
    .type = RR_TXT,
    .name = self->service_link,
    .data.TXT = &txt_data[0],
    .rr_class = RR_IN,
    .msbit = 1,
    .ttl = (type == MDNS_ANNOUNCE_GOODBYE) ? 0 : 120,
    .next = &answers[SRV_INDEX]
  };

  answers[SRV_INDEX] = (struct rr_entry) {
    .type = RR_SRV,
    .name = self->service_link,
    .data.SRV.port = port,
    .data.SRV.priority = 0,
    .data.SRV.weight = 0,
    .data.SRV.target = self->domain_name,
    .rr_class = RR_IN,
    .msbit = 1,
    .ttl = (type == MDNS_ANNOUNCE_GOODBYE) ? 0 : 120,
    .next = &answers[A_AAAA_INDEX]
  };

  answers[A_AAAA_INDEX] = (struct rr_entry) {
    .name = self->domain_name,
    .rr_class = RR_IN,
    .msbit = 1,
    .ttl = (type == MDNS_ANNOUNCE_GOODBYE) ? 0 : 120,
    .next = NULL
  };

  if (addr->sa_family == AF_INET) {
    answers[A_AAAA_INDEX].type = RR_A;
    memcpy (&answers[A_AAAA_INDEX].data.A.addr,
        &((struct sockaddr_in *) addr)->sin_addr,
        sizeof (answers[A_AAAA_INDEX].data.A.addr));
  } else {
    answers[A_AAAA_INDEX].type = RR_AAAA;
    memcpy (&answers[A_AAAA_INDEX].data.AAAA.addr,
        &((struct sockaddr_in6 *) addr)->sin6_addr,
        sizeof (answers[A_AAAA_INDEX].data.AAAA.addr));
  }

  GST_LOG_OBJECT (self, "Sending answers");
  mdns_entries_send (self->ctx, &hdr, answers);
}

static void
_mdns_cb (ClapperControlHubMdns *self, const struct sockaddr *addr,
    const char* service, enum mdns_announce_type type)
{
  /* We should respond when service is NULL too (e.g. in INITIAL announce) */
  if (service && strcmp (service, CLAPPER_CONTROL_HUB_MDNS_SERVICE) != 0)
    return;

  GST_LOG_OBJECT (self, "Handling announcement type: %s",
      (type == MDNS_ANNOUNCE_RESPONSE) ? "RESPONSE"
      : (type == MDNS_ANNOUNCE_INITIAL) ? "INITIAL" : "GOODBYE");

  _send_entries (self, addr, type);
}

static gboolean
mdns_stop_cb (ClapperControlHubMdns *self)
{
  return (g_atomic_int_get (&self->run) == 0);
}

static void
_post_error_str (ClapperControlHubMdns *self, const gchar *err_str)
{
  ClapperReactable *reactable;

  reactable = CLAPPER_REACTABLE_CAST (gst_object_get_parent (GST_OBJECT_CAST (self)));
  if (G_LIKELY (reactable != NULL)) {
    ClapperPlayer *player = clapper_reactable_get_player (reactable);

    if (G_LIKELY (player != NULL)) {
      GstStructure *structure;
      GError *error;

      /* Post error message with reactable set as source */
      error = g_error_new (G_IO_ERROR, G_IO_ERROR_FAILED, "%s", err_str);
      structure = gst_structure_new ("enhancer-error",
          "error", G_TYPE_ERROR, error, NULL);
      clapper_player_post_message (player,
          gst_message_new_application (GST_OBJECT_CAST (reactable), structure),
          CLAPPER_PLAYER_MESSAGE_DESTINATION_APPLICATION);

      g_error_free (error);
      gst_object_unref (player);
    }

    gst_object_unref (reactable);
  }
}

static gboolean
_serve_in_thread_func (ClapperControlHubMdns *self)
{
  if (!self->initialized) {
    int resp;
    char err_str[128];
    const gchar *host_name, *prgname, *cch_name;

    GST_DEBUG_OBJECT (self, "Initializing");

    host_name = g_get_host_name ();
    if (!(prgname = g_get_prgname ()))
      prgname = "unknown";
    cch_name = GST_OBJECT_NAME (self);

    if (!self->domain_name)
      self->domain_name = g_strdup_printf ("%s.local", host_name);
    if (!self->service_link) {
      self->service_link = g_strdup_printf ("%s %s %s.%s",
          host_name, prgname, cch_name, CLAPPER_CONTROL_HUB_MDNS_SERVICE);
    }
    if (!self->txt_records) {
      GStrvBuilder *builder = g_strv_builder_new ();
      gchar *id_txt, *app_txt;

      id_txt = g_strdup_printf ("id=%s-%s-%s", host_name, prgname, cch_name);
      app_txt = g_strdup_printf ("app=%s", prgname);

      /* XXX: When entries are added/removed here, "N_TXT" must be updated.
       * Also, note that these are sent in reverse order. */
      g_strv_builder_add (builder, id_txt);
      g_strv_builder_add (builder, "chver=" CONTROL_HUB_VERSION_S);
      g_strv_builder_add (builder, "cver=" CLAPPER_VERSION_S);
      g_strv_builder_add (builder, app_txt);

      self->txt_records = g_strv_builder_end (builder);
      g_strv_builder_unref (builder);

      g_free (id_txt);
      g_free (app_txt);
    }

    if ((resp = mdns_init (&self->ctx, MDNS_ADDR_IPV4, MDNS_PORT)) < 0) {
      mdns_strerror (resp, err_str, sizeof (err_str));
      GST_ERROR_OBJECT (self, "Could not initialize MDNS, reason: %s", err_str);
      _post_error_str (self, err_str);
    } else {
      mdns_announce (self->ctx, RR_PTR, (mdns_announce_callback) _mdns_cb, self);
      self->initialized = TRUE;
      GST_DEBUG_OBJECT (self, "Initialized");
    }
  }
  if (self->initialized) {
    int resp;
    char err_str[128];

    GST_INFO_OBJECT (self, "Serving");

    /* NOTE: This function blocks (runs loop internally) */
    if ((resp = mdns_serve (self->ctx, (mdns_stop_func) mdns_stop_cb, self)) < 0) {
      mdns_strerror (resp, err_str, sizeof (err_str));
      GST_ERROR_OBJECT (self, "Could start MDNS, reason: %s", err_str);
      _post_error_str (self, err_str);
    }

    GST_INFO_OBJECT (self, "Stopped");
  }

  return G_SOURCE_REMOVE;
}

ClapperControlHubMdns *
clapper_control_hub_mdns_new (void)
{
  ClapperControlHubMdns *mdns;
  gchar name[24];

  g_snprintf (name, sizeof (name), "controlhub%i", g_atomic_int_add (&_cch_id, 1));
  mdns = g_object_new (CLAPPER_TYPE_CONTROL_HUB_MDNS, "name", name, NULL);

  return gst_object_ref_sink (mdns);
}

void
clapper_control_hub_mdns_start (ClapperControlHubMdns *self, gint port)
{
  GMainContext *context = clapper_threaded_object_get_context (CLAPPER_THREADED_OBJECT_CAST (self));

  g_atomic_int_set (&self->port, port);
  g_atomic_int_set (&self->run, 1);

  g_main_context_invoke (context, (GSourceFunc) _serve_in_thread_func, self);
}

void
clapper_control_hub_mdns_stop (ClapperControlHubMdns *self)
{
  g_atomic_int_set (&self->run, 0);
}

static void
clapper_control_hub_mdns_thread_stop (ClapperThreadedObject *threaded_object)
{
  ClapperControlHubMdns *self = CLAPPER_CONTROL_HUB_MDNS_CAST (threaded_object);

  if (self->initialized) {
    GST_TRACE_OBJECT (self, "Destroy");
    mdns_destroy (self->ctx);
  }

  /* Always free, might be set even
   * if initialization fails */
  g_free (self->domain_name);
  g_free (self->service_link);
  g_strfreev (self->txt_records);
}

static void
clapper_control_hub_mdns_init (ClapperControlHubMdns *self)
{
}

static void
clapper_control_hub_mdns_dispose (GObject *object)
{
  ClapperControlHubMdns *self = CLAPPER_CONTROL_HUB_MDNS_CAST (object);

  clapper_control_hub_mdns_stop (self);

  G_OBJECT_CLASS (parent_class)->dispose (object);
}

static void
clapper_control_hub_mdns_finalize (GObject *object)
{
  ClapperControlHubMdns *self = CLAPPER_CONTROL_HUB_MDNS_CAST (object);

  GST_TRACE_OBJECT (self, "Finalize");

  G_OBJECT_CLASS (parent_class)->finalize (object);
}

static void
clapper_control_hub_mdns_class_init (ClapperControlHubMdnsClass *klass)
{
  GObjectClass *gobject_class = (GObjectClass *) klass;
  ClapperThreadedObjectClass *threaded_object = (ClapperThreadedObjectClass *) klass;

  GST_DEBUG_CATEGORY_INIT (GST_CAT_DEFAULT, "clappercontrolhubmdns",
      GST_DEBUG_FG_RED, "Clapper Control Hub MDNS");

  gobject_class->dispose = clapper_control_hub_mdns_dispose;
  gobject_class->finalize = clapper_control_hub_mdns_finalize;

  threaded_object->thread_stop = clapper_control_hub_mdns_thread_stop;
}
