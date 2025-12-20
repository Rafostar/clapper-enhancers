/*
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

#define GST_CAT_DEFAULT clapper_media_scanner_debug
GST_DEBUG_CATEGORY_STATIC (GST_CAT_DEFAULT);

#define CLAPPER_TYPE_MEDIA_SCANNER (clapper_media_scanner_get_type())
#define CLAPPER_MEDIA_SCANNER_CAST(obj) ((ClapperMediaScanner *)(obj))
G_DECLARE_FINAL_TYPE (ClapperMediaScanner, clapper_media_scanner, CLAPPER, MEDIA_SCANNER, ClapperThreadedObject);

G_MODULE_EXPORT void peas_register_types (PeasObjectModule *module);

struct _ClapperMediaScanner
{
  ClapperThreadedObject parent;

  GstElement *pipeline;
  GstElement *urisourcebin;
  GstElement *mqueue;
  GstBus *bus;

  /* Used from scanner thread */
  gboolean running;
  GstTagList *collected_tags;
  gboolean stream_tags_allowed;

  /* Used with a mutex lock */
  GSource *timeout_source;
  GPtrArray *pending_items;
  ClapperMediaItem *scanned_item;
};

static inline void
_start_pipeline (ClapperMediaScanner *self)
{
  if (!self->running) {
    gst_bus_set_flushing (self->bus, FALSE);
    if ((self->running = (gst_element_set_state (
        self->pipeline, GST_STATE_PAUSED) != GST_STATE_CHANGE_FAILURE))) {
      GST_INFO_OBJECT (self, "Media scanner pipeline started");
    } else {
      GST_ERROR_OBJECT (self, "Media scanner pipeline could not start");
      gst_bus_set_flushing (self->bus, TRUE); // Keep flushing on failure
    }
  }
}

static inline void
_stop_pipeline (ClapperMediaScanner *self)
{
  if (self->running) {
    /* Drop pending messages, so they will not arrive after item got changed */
    gst_bus_set_flushing (self->bus, TRUE);
    if ((self->running = (gst_element_set_state (
        self->pipeline, GST_STATE_READY) == GST_STATE_CHANGE_FAILURE))) {
      GST_ERROR_OBJECT (self, "Media scanner pipeline could not be stopped");
    } else {
      GST_INFO_OBJECT (self, "Media scanner pipeline stopped");
    }
  }
}

/* Should be run from scanner thread only */
static void
_scan_next_item (ClapperMediaScanner *self)
{
  ClapperMediaItem *item = NULL;
  GstTagList *tags;
  const gchar *uri;
  gboolean empty_tags;

  _stop_pipeline (self);

  GST_OBJECT_LOCK (self);

  if (self->pending_items->len > 0)
    item = g_ptr_array_steal_index (self->pending_items, 0);

  GST_OBJECT_UNLOCK (self);

  if (!item) {
    GST_DEBUG_OBJECT (self, "No more pending items");
    return;
  }

  GST_DEBUG_OBJECT (self, "Investigating scan of %" GST_PTR_FORMAT, item);

  tags = clapper_media_item_get_tags (item);
  empty_tags = gst_tag_list_is_empty (tags);
  gst_tag_list_unref (tags);

  if (!empty_tags) {
    GST_DEBUG_OBJECT (self, "Queued %" GST_PTR_FORMAT
        " already has tags, ignoring media scan", item);
    goto finish;
  }

  uri = clapper_media_item_get_uri (item);
  GST_DEBUG_OBJECT (self, "Starting scan of %" GST_PTR_FORMAT "(%s)", item, uri);

  self->stream_tags_allowed = FALSE;
  gst_tag_list_take (&self->collected_tags, gst_tag_list_new_empty ());
  gst_tag_list_set_scope (self->collected_tags, GST_TAG_SCOPE_GLOBAL);
  g_object_set (self->urisourcebin, "uri", uri, NULL);

  GST_OBJECT_LOCK (self);
  gst_object_replace ((GstObject **) &self->scanned_item, GST_OBJECT_CAST (item));
  GST_OBJECT_UNLOCK (self);

  /* Sets "self->running" to TRUE on success */
  _start_pipeline (self);

finish:
  gst_clear_object (&item);

  /* If scan did not start for this item, try next */
  if (!self->running)
    _scan_next_item (self);
}

static void
_unqueue_item_scan (ClapperMediaScanner *self, ClapperMediaItem *item)
{
  GST_OBJECT_LOCK (self);

  /* Remove item that is either already scanned or queued to be */
  if (item == self->scanned_item) {
    GST_DEBUG_OBJECT (self, "Ignoring scan of current item %" GST_PTR_FORMAT, item);
    gst_clear_object (&self->scanned_item);
  } else {
    guint index = 0;

    if (g_ptr_array_find (self->pending_items, item, &index)) {
      GST_DEBUG_OBJECT (self, "Removing pending item %" GST_PTR_FORMAT, item);
      g_ptr_array_remove_index (self->pending_items, index);
    }
  }

  GST_OBJECT_UNLOCK (self);
}

/* Call with a lock */
static inline void
_clear_timeout_source (ClapperMediaScanner *self)
{
  if (self->timeout_source) {
    g_source_destroy (self->timeout_source);
    g_clear_pointer (&self->timeout_source, g_source_unref);
  }
}

static gboolean
_scan_next_item_delayed_cb (ClapperMediaScanner *self)
{
  GST_DEBUG_OBJECT (self, "Delayed scan handler reached");

  GST_OBJECT_LOCK (self);
  _clear_timeout_source (self);
  GST_OBJECT_UNLOCK (self);

  /* If already running, next item will
   * be scanned after that run finishes */
  if (!self->running)
    _scan_next_item (self);

  return G_SOURCE_REMOVE;
}

static GSource *
_context_timeout_add_full (GMainContext *context, gint priority, guint interval,
    GSourceFunc func, gpointer data, GDestroyNotify destroy_func)
{
  GSource *source = g_timeout_source_new (interval);

  g_source_set_priority (source, priority);
  g_source_set_callback (source, func, data, destroy_func);
  g_source_attach (source, context);

  return source;
}

static void
clapper_media_scanner_played_item_changed (ClapperReactable *reactable, ClapperMediaItem *item)
{
  ClapperMediaScanner *self = CLAPPER_MEDIA_SCANNER_CAST (reactable);

  GST_DEBUG_OBJECT (self, "Played item changed to: %" GST_PTR_FORMAT, item);
  _unqueue_item_scan (self, item);
}

static void
clapper_media_scanner_queue_item_added (ClapperReactable *reactable, ClapperMediaItem *item, guint index)
{
  ClapperMediaScanner *self = CLAPPER_MEDIA_SCANNER_CAST (reactable);

  GST_DEBUG_OBJECT (self, "Queue item added %" GST_PTR_FORMAT, item);

  GST_OBJECT_LOCK (self);

  g_ptr_array_add (self->pending_items, gst_object_ref (item));

  /* Schedule a scan. When multiple items are being added,
   * collect them and try to invoke scanner thread just once. */
  _clear_timeout_source (self);
  self->timeout_source = _context_timeout_add_full (
      clapper_threaded_object_get_context (CLAPPER_THREADED_OBJECT_CAST (self)),
      G_PRIORITY_DEFAULT_IDLE, 50,
      (GSourceFunc) _scan_next_item_delayed_cb,
      self, NULL);

  GST_OBJECT_UNLOCK (self);
}

static void
clapper_media_scanner_queue_item_removed (ClapperReactable *reactable, ClapperMediaItem *item, guint index)
{
  ClapperMediaScanner *self = CLAPPER_MEDIA_SCANNER_CAST (reactable);

  GST_DEBUG_OBJECT (self, "Queue item removed %" GST_PTR_FORMAT, item);
  _unqueue_item_scan (self, item);
}

static void
_discard_all_pending_items (ClapperMediaScanner *self)
{
  GST_DEBUG_OBJECT (self, "Discarding all pending items");

  GST_OBJECT_LOCK (self);

  /* If scan is scheduled, cancel it */
  _clear_timeout_source (self);

  /* Remove both item that is already scanned and all that are queued to be.
   * Do not stop pipeline from this thread, let it finish and result will be ignored. */
  gst_clear_object (&self->scanned_item);
  if (self->pending_items->len > 0)
    g_ptr_array_remove_range (self->pending_items, 0, self->pending_items->len);

  GST_OBJECT_UNLOCK (self);
}

static void
clapper_media_scanner_queue_cleared (ClapperReactable *reactable)
{
  ClapperMediaScanner *self = CLAPPER_MEDIA_SCANNER_CAST (reactable);

  GST_DEBUG_OBJECT (self, "Queue cleared");
  _discard_all_pending_items (self);
}

static void
clapper_media_scanner_reactable_iface_init (ClapperReactableInterface *iface)
{
  iface->played_item_changed = clapper_media_scanner_played_item_changed;
  iface->queue_item_added = clapper_media_scanner_queue_item_added;
  iface->queue_item_removed = clapper_media_scanner_queue_item_removed;
  iface->queue_cleared = clapper_media_scanner_queue_cleared;
}

#define parent_class clapper_media_scanner_parent_class
G_DEFINE_TYPE_WITH_CODE (ClapperMediaScanner, clapper_media_scanner, CLAPPER_TYPE_THREADED_OBJECT,
    G_IMPLEMENT_INTERFACE (CLAPPER_TYPE_REACTABLE, clapper_media_scanner_reactable_iface_init));

static inline void
_handle_element_msg (GstMessage *msg, ClapperMediaScanner *self)
{
  if (gst_message_has_name (msg, "ClapperPlaylistParsed")) {
    GstMessage *playlist_msg = NULL;

    GST_OBJECT_LOCK (self);

    /* Scanner knows which media item it scans, so set it in message.
     * A "scanned_item" is set to non-NULL value only from scanner
     * thread (this one) which ensures that correct item is used here. */
    if (self->scanned_item) {
      const GstStructure *src_structure = gst_message_get_structure (msg);
      GstStructure *dest_structure;

      GST_DEBUG_OBJECT (self, "Resolved %" GST_PTR_FORMAT
          "(%s) into a playlist", self->scanned_item, clapper_media_item_get_uri (self->scanned_item));

      dest_structure = gst_structure_copy (src_structure);
      gst_structure_set (dest_structure,
          "item", CLAPPER_TYPE_MEDIA_ITEM, self->scanned_item, NULL);
      playlist_msg = gst_message_new_application (GST_OBJECT_CAST (self), dest_structure);
    }

    GST_OBJECT_UNLOCK (self);

    if (playlist_msg) {
      ClapperPlayer *player = clapper_reactable_get_player (CLAPPER_REACTABLE_CAST (self));

      /* Just forward message to the player with scanned item set. No need to
       * rescan it, as player keeps expanding single pipeline until video plays,
       * at which point scanner uses collected tags and updates item to final values. */
      if (G_LIKELY (player != NULL)) {
        clapper_player_post_message (player, playlist_msg,
            CLAPPER_PLAYER_MESSAGE_DESTINATION_PLAYER);
        gst_object_unref (player);
      } else {
        gst_message_unref (playlist_msg);
      }
    }
  }
}

static inline void
_handle_tag_msg (GstMessage *msg, ClapperMediaScanner *self)
{
  GstObject *src = GST_MESSAGE_SRC (msg);
  GstTagList *tags = NULL;

  if (G_UNLIKELY (!src))
    return;

  gst_message_parse_tag (msg, &tags);

  /* Global tags are always prioritized.
   * Only use stream tags as fallback when allowed. */
  if (gst_tag_list_get_scope (tags) == GST_TAG_SCOPE_GLOBAL) {
    GST_LOG_OBJECT (self, "Got GLOBAL tags from element: %s: %" GST_PTR_FORMAT,
        GST_OBJECT_NAME (src), tags);
    gst_tag_list_insert (self->collected_tags, tags, GST_TAG_MERGE_REPLACE);
  } else if (self->stream_tags_allowed) {
    GST_LOG_OBJECT (self, "Got STREAM tags from element: %s: %" GST_PTR_FORMAT,
        GST_OBJECT_NAME (src), tags);
    gst_tag_list_insert (self->collected_tags, tags, GST_TAG_MERGE_KEEP);
  }

  gst_tag_list_unref (tags);
}

static inline void
_handle_stream_collection_msg (GstMessage *msg, ClapperMediaScanner *self)
{
  GstStreamCollection *collection = NULL;
  guint i, n_streams, n_video = 0, n_audio = 0, n_text = 0;

  GST_DEBUG_OBJECT (self, "Stream collection");

  gst_message_parse_stream_collection (msg, &collection);
  n_streams = gst_stream_collection_get_size (collection);

  for (i = 0; i < n_streams; ++i) {
    GstStream *stream = gst_stream_collection_get_stream (collection, i);
    GstStreamType stream_type = gst_stream_get_stream_type (stream);

    if ((stream_type & GST_STREAM_TYPE_VIDEO) == GST_STREAM_TYPE_VIDEO)
      n_video++;
    else if ((stream_type & GST_STREAM_TYPE_AUDIO) == GST_STREAM_TYPE_AUDIO)
      n_audio++;
    else if ((stream_type & GST_STREAM_TYPE_TEXT) == GST_STREAM_TYPE_TEXT)
      n_text++;
  }
  gst_object_unref (collection);

  self->stream_tags_allowed = (n_video + n_audio + n_text == 1);
  GST_DEBUG_OBJECT (self, "Stream tags allowed: %s", (self->stream_tags_allowed) ? "yes" : "no");
}

static inline void
_handle_async_done_msg (GstMessage *msg, ClapperMediaScanner *self)
{
  ClapperMediaItem *item;

  GST_DEBUG_OBJECT (self, "Async done");

  /* A "scanned_item" is set to non-NULL value only from
   * scanner thread (this one), so reading it is not racy here */
  GST_OBJECT_LOCK (self);
  item = g_steal_pointer (&self->scanned_item);
  GST_OBJECT_UNLOCK (self);

  /* Can be NULL if removed while scan of it was running */
  if (item) {
    GST_DEBUG_OBJECT (self, "Finished scan of %" GST_PTR_FORMAT, item);
    clapper_media_item_populate_tags (item, self->collected_tags);
    gst_object_unref (item);
  }

  /* Try to scan next item */
  _scan_next_item (self);
}

static inline void
_handle_latency_msg (GstMessage *msg G_GNUC_UNUSED, ClapperMediaScanner *self)
{
  GST_LOG_OBJECT (self, "Latency changed");
  gst_bin_recalculate_latency (GST_BIN_CAST (self->pipeline));
}

static inline void
_handle_warning_msg (GstMessage *msg, ClapperMediaScanner *self)
{
  GError *error = NULL;

  gst_message_parse_warning (msg, &error, NULL);
  GST_WARNING_OBJECT (self, "Warning: %s", error->message);
  g_error_free (error);
}

static inline void
_handle_error_msg (GstMessage *msg, ClapperMediaScanner *self)
{
  GError *error = NULL;

  gst_message_parse_error (msg, &error, NULL);
  GST_ERROR_OBJECT (self, "Error: %s", error->message);
  g_error_free (error);

  /* After error we should go to READY, so all elements will stop processing
   * buffers then move to the next item. This function does both. */
  _scan_next_item (self);
}

static gboolean
_bus_message_func (GstBus *bus, GstMessage *msg, ClapperMediaScanner *self)
{
  switch (GST_MESSAGE_TYPE (msg)) {
    case GST_MESSAGE_ELEMENT:
      _handle_element_msg (msg, self);
      break;
    case GST_MESSAGE_TAG:
      _handle_tag_msg (msg, self);
      break;
    case GST_MESSAGE_STREAM_COLLECTION:
      _handle_stream_collection_msg (msg, self);
      break;
    case GST_MESSAGE_ASYNC_DONE:
      _handle_async_done_msg (msg, self);
      break;
    case GST_MESSAGE_LATENCY:
      _handle_latency_msg (msg, self);
      break;
    case GST_MESSAGE_WARNING:
      _handle_warning_msg (msg, self);
      break;
    case GST_MESSAGE_ERROR:
      _handle_error_msg (msg, self);
      break;
    default:
      break;
  }

  return G_SOURCE_CONTINUE;
}

static void
_deep_element_added_cb (GstBin *urisourcebin, GstBin *sub_bin,
    GstElement *element, ClapperMediaScanner *self)
{
  GstElementFactory *factory = gst_element_get_factory (element);
  const gchar *factory_name;

  if (G_UNLIKELY (factory == NULL))
    return;

  factory_name = g_intern_static_string (GST_OBJECT_NAME (factory));
  GST_LOG_OBJECT (self, "Deep element added: %s", factory_name);

  if (factory_name == g_intern_static_string ("clapperextractablesrc")
      || factory_name == g_intern_static_string ("clapperplaylistdemux")) {
    ClapperPlayer *player = clapper_reactable_get_player (CLAPPER_REACTABLE_CAST (self));

    if (G_LIKELY (player != NULL)) {
      g_object_set (element,
          "enhancer-proxies", clapper_player_get_enhancer_proxies (player),
          NULL);
      gst_object_unref (player);
    }
  }
}

static void
_pad_added_cb (GstElement *urisourcebin, GstPad *pad, ClapperMediaScanner *self)
{
  GstElement *fakesink;
  GstPad *mqueue_sink_pad, *mqueue_src_pad, *fakesink_pad;
  gchar sink_pad_name[32];

  GST_TRACE_OBJECT (self, "Pad added in urisourcebin: %" GST_PTR_FORMAT, pad);

  fakesink = gst_element_factory_make ("fakesink", NULL);
  gst_bin_add (GST_BIN_CAST (self->pipeline), fakesink);

  g_snprintf (sink_pad_name, sizeof (sink_pad_name), "sink_%s", GST_PAD_NAME (pad) + 4);
  mqueue_sink_pad = gst_element_request_pad (self->mqueue,
      gst_element_get_pad_template (self->mqueue, "sink_%u"),
      sink_pad_name, NULL);

  if (G_UNLIKELY (gst_pad_link (pad, mqueue_sink_pad) != GST_PAD_LINK_OK))
    GST_ERROR_OBJECT (self, "Could not link \"urisourcebin\" to \"multiqueue\"");

  gst_object_unref (mqueue_sink_pad);

  mqueue_src_pad = gst_element_get_static_pad (self->mqueue, GST_PAD_NAME (pad));
  fakesink_pad = gst_element_get_static_pad (fakesink, "sink");

  if (G_UNLIKELY (gst_pad_link (mqueue_src_pad, fakesink_pad) != GST_PAD_LINK_OK))
    GST_ERROR_OBJECT (self, "Could not link \"multiqueue\" to \"fakesink\"");

  gst_object_unref (mqueue_src_pad);
  gst_object_unref (fakesink_pad);

  gst_element_sync_state_with_parent (fakesink);
}

static void
_pad_removed_cb (GstElement *urisourcebin, GstPad *pad, ClapperMediaScanner *self)
{
  GstElement *fakesink;
  GstPad *mqueue_sink_pad, *mqueue_src_pad, *fakesink_pad;
  gchar sink_pad_name[32];

  GST_TRACE_OBJECT (self, "Pad removed in urisourcebin: %" GST_PTR_FORMAT, pad);

  mqueue_src_pad = gst_element_get_static_pad (self->mqueue, GST_PAD_NAME (pad));
  fakesink_pad = gst_pad_get_peer (mqueue_src_pad);
  fakesink = gst_pad_get_parent_element (fakesink_pad);

  GST_TRACE_OBJECT (self, "Removing %" GST_PTR_FORMAT, fakesink);

  if (G_UNLIKELY (!gst_pad_unlink (mqueue_src_pad, fakesink_pad)))
    GST_ERROR_OBJECT (self, "Could not unlink \"multiqueue\" from \"fakesink\"");

  gst_object_unref (mqueue_src_pad);
  gst_object_unref (fakesink_pad);

  g_snprintf (sink_pad_name, sizeof (sink_pad_name), "sink_%s", GST_PAD_NAME (pad) + 4);
  mqueue_sink_pad = gst_element_get_static_pad (self->mqueue, sink_pad_name);
  gst_element_release_request_pad (self->mqueue, mqueue_sink_pad);
  gst_object_unref (mqueue_sink_pad);

  gst_element_set_state (fakesink, GST_STATE_NULL);
  gst_bin_remove (GST_BIN_CAST (self->pipeline), fakesink);
  gst_object_unref (fakesink);
}

static void
clapper_media_scanner_thread_start (ClapperThreadedObject *threaded_object)
{
  ClapperMediaScanner *self = CLAPPER_MEDIA_SCANNER_CAST (threaded_object);

  GST_DEBUG_OBJECT (self, "Preparing pipeline");

  if (!(self->urisourcebin = gst_element_factory_make ("urisourcebin", NULL))) {
    GST_ERROR_OBJECT (self, "Could not create \"urisourcebin\" element");
    return;
  }
  if (!(self->mqueue = gst_element_factory_make ("multiqueue", NULL))) {
    GST_ERROR_OBJECT (self, "Could not create \"multiqueue\" element");
    gst_clear_object (&self->urisourcebin);
    return;
  }

  g_object_set (self->urisourcebin, "parse-streams", TRUE, NULL);
  g_signal_connect (self->urisourcebin, "deep-element-added", G_CALLBACK (_deep_element_added_cb), self);
  g_signal_connect (self->urisourcebin, "pad-added", G_CALLBACK (_pad_added_cb), self);
  g_signal_connect (self->urisourcebin, "pad-removed", G_CALLBACK (_pad_removed_cb), self);

  self->pipeline = gst_pipeline_new ("clapper-media-scanner");
  gst_bin_add_many (GST_BIN_CAST (self->pipeline), self->urisourcebin, self->mqueue, NULL);

  GST_TRACE_OBJECT (self, "Created pipeline: %" GST_PTR_FORMAT, self->pipeline);

  self->bus = gst_element_get_bus (self->pipeline);
  gst_bus_add_watch (self->bus, (GstBusFunc) _bus_message_func, self);
}

static void
clapper_media_scanner_thread_stop (ClapperThreadedObject *threaded_object)
{
  ClapperMediaScanner *self = CLAPPER_MEDIA_SCANNER_CAST (threaded_object);

  if (self->bus) {
    gst_bus_set_flushing (self->bus, TRUE);
    gst_bus_remove_watch (self->bus);
  }
  if (self->pipeline)
    gst_element_set_state (self->pipeline, GST_STATE_NULL);

  gst_clear_object (&self->bus);
  gst_clear_object (&self->pipeline);
}

static void
clapper_media_scanner_init (ClapperMediaScanner *self)
{
  self->pending_items = g_ptr_array_new_with_free_func ((GDestroyNotify) gst_object_unref);
}

static void
clapper_media_scanner_dispose (GObject *object)
{
  ClapperMediaScanner *self = CLAPPER_MEDIA_SCANNER_CAST (object);

  _discard_all_pending_items (self);

  G_OBJECT_CLASS (parent_class)->dispose (object);
}

static void
clapper_media_scanner_finalize (GObject *object)
{
  ClapperMediaScanner *self = CLAPPER_MEDIA_SCANNER_CAST (object);

  GST_TRACE_OBJECT (self, "Finalize");

  g_ptr_array_unref (self->pending_items);

  G_OBJECT_CLASS (parent_class)->finalize (object);
}

static void
clapper_media_scanner_class_init (ClapperMediaScannerClass *klass)
{
  GObjectClass *gobject_class = (GObjectClass *) klass;
  ClapperThreadedObjectClass *threaded_object = (ClapperThreadedObjectClass *) klass;

  GST_DEBUG_CATEGORY_INIT (GST_CAT_DEFAULT, "clappermediascanner", 0,
      "Clapper Media Scanner");

  gobject_class->dispose = clapper_media_scanner_dispose;
  gobject_class->finalize = clapper_media_scanner_finalize;

  threaded_object->thread_start = clapper_media_scanner_thread_start;
  threaded_object->thread_stop = clapper_media_scanner_thread_stop;
}

void
peas_register_types (PeasObjectModule *module)
{
  peas_object_module_register_extension_type (module, CLAPPER_TYPE_REACTABLE, CLAPPER_TYPE_MEDIA_SCANNER);
}
