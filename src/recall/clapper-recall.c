/* Clapper Enhancer Recall
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

/**
 * ClapperRecall:
 *
 * An enhancer responsible for "recalling" where playback left off.
 */

#include "config.h"

#include <glib.h>
#include <glib-object.h>
#include <gio/gio.h>
#include <gmodule.h>
#include <libpeas.h>
#include <clapper/clapper.h>

#include <sqlite3.h>

#define DEFAULT_PERSISTENT_STORAGE TRUE
#define DEFAULT_MARK_POSITION TRUE
#define DEFAULT_AUTO_RESUME TRUE
#define DEFAULT_MIN_DURATION 180
#define DEFAULT_MIN_ELAPSED 60
#define DEFAULT_MIN_REMAINING 60

#define CLAPPER_RECALL_SECONDS_TO_USECONDS(seconds) ((gint64) (seconds * G_GINT64_CONSTANT (1000000)))
#define CLAPPER_RECALL_USECONDS_TO_SECONDS(useconds) ((gdouble) useconds / G_GINT64_CONSTANT (1000000))

#define RECALL_MARKER_TYPE CLAPPER_MARKER_TYPE_CUSTOM_2
#define CHUNK_SIZE 4096

#define GST_CAT_DEFAULT clapper_recall_debug
GST_DEBUG_CATEGORY_STATIC (GST_CAT_DEFAULT);

#define CLAPPER_TYPE_RECALL (clapper_recall_get_type())
#define CLAPPER_RECALL_CAST(obj) ((ClapperRecall *)(obj))
G_DECLARE_FINAL_TYPE (ClapperRecall, clapper_recall, CLAPPER, RECALL, GstObject);

G_MODULE_EXPORT void peas_register_types (PeasObjectModule *module);

typedef struct
{
  gatomicrefcount refcount;
  gchar *hash; // set and get in enhancer thread only
  ClapperMediaItem *item;
  gdouble position;
  gdouble duration;
  ClapperMarker *marker;
} ClapperRecallMemo;

typedef struct
{
  ClapperRecall *recall;
  ClapperRecallMemo *memo;
  gchar *pending_hash;
} ClapperRecallData;

struct _ClapperRecall
{
  GstObject parent;

  GMainContext *context;
  GThreadPool *pool;

  sqlite3 *db;
  gboolean db_ensured;

  sqlite3_stmt *memorize_stmt;
  sqlite3_stmt *recall_stmt;

  GPtrArray *memos;
  ClapperRecallMemo *current_memo;

  ClapperPlayerState state;
  gdouble played_position;
  gboolean resume_done;

  gboolean persistent_storage;
  gboolean mark_position;
  gboolean auto_resume;
  gint min_duration;
  gint min_elapsed;
  gint min_remaining;
};

enum
{
  PROP_0,
  PROP_PERSISTENT_STORAGE,
  PROP_MARK_POSITION,
  PROP_AUTO_RESUME,
  PROP_MIN_DURATION,
  PROP_MIN_ELAPSED,
  PROP_MIN_REMAINING,
  PROP_LAST
};

static GParamSpec *param_specs[PROP_LAST] = { NULL, };

static ClapperRecallMemo *
clapper_recall_memo_new_for_item (ClapperMediaItem *item)
{
  ClapperRecallMemo *memo = g_new0 (ClapperRecallMemo, 1);

  g_atomic_ref_count_init (&memo->refcount);
  memo->item = gst_object_ref (item);
  memo->position = -1;

  GST_TRACE ("Created memo for %" GST_PTR_FORMAT, memo->item);

  return memo;
}

static ClapperRecallMemo *
clapper_recall_memo_ref (ClapperRecallMemo *memo)
{
  g_atomic_ref_count_inc (&memo->refcount);
  return memo;
}

static void
clapper_recall_memo_unref (ClapperRecallMemo *memo)
{
  if (!g_atomic_ref_count_dec (&memo->refcount))
    return;

  GST_TRACE ("Freeing memo for %" GST_PTR_FORMAT, memo->item);

  g_free (memo->hash);
  gst_object_unref (memo->item);
  gst_clear_object (&memo->marker);

  g_free (memo);
}

static ClapperRecallData *
clapper_recall_data_new_take (ClapperRecall *self, ClapperRecallMemo *memo, gchar *pending_hash)
{
  ClapperRecallData *data = g_new (ClapperRecallData, 1);

  data->recall = self;
  data->memo = memo; // take object reference
  data->pending_hash = pending_hash; // take generated hash

  GST_TRACE ("Created recall data for memo with item %" GST_PTR_FORMAT, data->memo->item);

  return data;
}

static void
clapper_recall_data_free (ClapperRecallData *data)
{
  GST_TRACE ("Freeing recall data for memo with item %" GST_PTR_FORMAT, data->memo->item);

  clapper_recall_memo_unref (data->memo); // unref taken object
  g_free (data->pending_hash); // free taken hash if its still there
  g_free (data);
}

static inline gchar *
_make_db_filename (ClapperRecall *self)
{
  GFile *data_dir;
  GError *error = NULL;
  gchar *data_dir_path, *db_filename;

  data_dir = g_file_new_build_filename (g_get_user_data_dir (),
      CLAPPER_API_NAME, "enhancers", "clapper-recall", NULL);

  if (!g_file_make_directory_with_parents (data_dir, NULL, &error)) {
    if (error->domain != G_IO_ERROR || error->code != G_IO_ERROR_EXISTS) {
      GST_ERROR_OBJECT (self, "Failed to create directory for DB: %s", error->message);
      g_clear_object (&data_dir);
    }
    g_error_free (error);
  }

  if (!data_dir) // when failed to create dir
    return NULL;

  data_dir_path = g_file_get_path (data_dir);
  g_object_unref (data_dir);

  db_filename = g_build_filename (data_dir_path, "recall.db", NULL);
  g_free (data_dir_path);

  return db_filename;
}

static void
_clean_db (ClapperRecall *self)
{
  gchar *errmsg = NULL;
  const gchar *sql_cmd =
      "DELETE FROM recall WHERE hash NOT IN ("
      "SELECT hash FROM recall ORDER BY updated DESC LIMIT 1000"
      ");";

  if (sqlite3_exec (self->db, sql_cmd, NULL, NULL, &errmsg) == SQLITE_OK) {
    GST_INFO_OBJECT (self, "Purged old DB entries");
  } else {
    GST_ERROR_OBJECT (self, "Failed to create table: %s", errmsg);
    sqlite3_free (errmsg);
  }
}

static void
_truncate_db (ClapperRecall *self)
{
  int rc, counter = 0;

  GST_LOG_OBJECT (self, "DB truncate start");

  do {
    rc = sqlite3_wal_checkpoint_v2 (self->db, "main", SQLITE_CHECKPOINT_TRUNCATE, NULL, NULL);
    if (rc == SQLITE_LOCKED) {
      GST_LOG_OBJECT (self, "Awaiting DB to be unlocked");
      g_usleep (1000);
    }
  } while (rc != SQLITE_OK && ++counter < 3);

  if (G_LIKELY (rc == SQLITE_OK))
    GST_LOG_OBJECT (self, "DB truncate finish");
  else
    GST_ERROR_OBJECT (self, "DB checkpoint failed: %s", sqlite3_errmsg (self->db));
}

static gboolean
_ensure_db (ClapperRecall *self)
{
  if (!self->db_ensured) {
    gchar *db_filename;

    if ((db_filename = _make_db_filename (self))) {
      gboolean had_error = FALSE;

      if (!(had_error = sqlite3_open (db_filename, &self->db) != SQLITE_OK)) {
        gchar *errmsg = NULL;
        const gchar *sql_cmd =
            "CREATE TABLE IF NOT EXISTS recall ("
            "hash TEXT PRIMARY KEY,"
            "position INTEGER,"
            "updated DATETIME DEFAULT CURRENT_TIMESTAMP"
            ");";

        if ((had_error = sqlite3_exec (self->db, sql_cmd, NULL, NULL, &errmsg) != SQLITE_OK)) {
          GST_ERROR_OBJECT (self, "Failed to create table: %s", errmsg);
          sqlite3_free (errmsg);
        }
      } else {
        GST_ERROR_OBJECT (self, "Failed to open DB: %s", sqlite3_errmsg (self->db));
      }

      if (!had_error) {
        sqlite3_exec (self->db, "PRAGMA synchronous=NORMAL;", NULL, NULL, NULL);
        sqlite3_exec (self->db, "PRAGMA journal_mode=WAL;", NULL, NULL, NULL);
        _truncate_db (self);
      } else {
        g_clear_pointer (&self->db, sqlite3_close);
      }

      g_free (db_filename);
    }

    self->db_ensured = TRUE;
  }

  return (self->db != NULL);
}

static inline gchar *
_generate_data_hash (ClapperRecall *self, ClapperRecallMemo *memo)
{
  const gchar *seekable_protos[] = { "file", "sftp", "ftp", "smb", "davs", "dav" };
  const gchar *uri;
  gchar *redirect_uri, *hash = NULL;
  guint proto_index;
  gboolean proto_found = FALSE;

  /* Prefer redirect as URI for hash generation */
  redirect_uri = clapper_media_item_get_redirect_uri (memo->item);
  uri = (redirect_uri) ? redirect_uri : clapper_media_item_get_uri (memo->item);

  for (proto_index = 0; proto_index < G_N_ELEMENTS (seekable_protos); ++proto_index) {
    if (gst_uri_has_protocol (uri, seekable_protos[proto_index])) {
      proto_found = TRUE;
      break;
    }
  }

  if (proto_found) {
    GFile *file;
    GFileInputStream *istream;
    GFileInfo *info;
    GError *error = NULL;
    gsize file_size = 0;

    GST_DEBUG_OBJECT (self, "Generating %" GST_PTR_FORMAT " hash from file data",
        memo->item);

    file = g_file_new_for_uri (uri);

    if ((info = g_file_query_info (file, G_FILE_ATTRIBUTE_STANDARD_SIZE, 0, NULL, &error))) {
      file_size = g_file_info_get_size (info);
      g_object_unref (info);
    } else {
      GST_ERROR_OBJECT (self, "Could not determine %" GST_PTR_FORMAT " file size, reason: %s",
          memo->item, error->message);
      g_clear_error (&error);
      g_object_unref (file);

      goto finish;
    }

    GST_LOG_OBJECT (self, "%" GST_PTR_FORMAT " file size: %" G_GSIZE_FORMAT,
        memo->item, file_size);

    /* NOTE: 0.8 since final seek is to 20% of file size */
    if (file_size < CHUNK_SIZE / 0.8) {
      GST_DEBUG_OBJECT (self, "Determined %" GST_PTR_FORMAT
          " file size is too small to seek in it", memo->item);
      g_object_unref (file);

      goto finish;
    }

    if (!(istream = g_file_read (file, NULL, &error))) {
      GST_ERROR_OBJECT (self, "Could not read %" GST_PTR_FORMAT
          " file, reason: %s", memo->item, error->message);
      g_clear_error (&error);
      g_object_unref (file);

      goto finish;
    }

    if (g_seekable_can_seek (G_SEEKABLE (istream))) {
      GChecksum *checksum = g_checksum_new (G_CHECKSUM_SHA256);
      const guint offsets[] = { 0, 10, 20 };
      gchar buffer[CHUNK_SIZE];
      guint i;
      gboolean success = TRUE;

      for (i = 0; i < G_N_ELEMENTS (offsets); ++i) {
        gsize bytes_read, offset = (file_size * offsets[i]) / 100;

        GST_LOG_OBJECT (self, "Reading %" GST_PTR_FORMAT " file at offset: %" G_GSIZE_FORMAT,
            memo->item, offset);

        if (offset > 0 && !g_seekable_seek (G_SEEKABLE (istream), offset, G_SEEK_SET, NULL, &error)) {
          GST_ERROR_OBJECT (self, "Could not seek in %" GST_PTR_FORMAT
              " file data, reason: %s", memo->item, error->message);
          g_clear_error (&error);

          success = FALSE;
          break;
        }

        bytes_read = g_input_stream_read (G_INPUT_STREAM (istream), buffer, CHUNK_SIZE, NULL, &error);
        if (error) {
          GST_ERROR_OBJECT (self, "Could not read %" GST_PTR_FORMAT
              " file data, reason: %s", memo->item, error->message);
          g_clear_error (&error);

          success = FALSE;
          break;
        }

        if (G_LIKELY (bytes_read == CHUNK_SIZE)) {
          g_checksum_update (checksum, (const guchar *) buffer, bytes_read);
        } else {
          success = FALSE;
          break;
        }
      }

      if (success)
        hash = g_strdup (g_checksum_get_string (checksum));

      g_checksum_free (checksum);
    } else {
      /* Not an error - server might not support seeking */
      GST_DEBUG_OBJECT (self, "%" GST_PTR_FORMAT " file is not seekable", memo->item);
    }

    g_object_unref (istream);
    g_object_unref (file);
  }

finish:
  g_free (redirect_uri);

  return hash;
}

static inline gchar *
_generate_uri_hash (ClapperRecall *self, ClapperRecallMemo *memo)
{
  GChecksum *checksum;
  gchar *redirect_uri, *hash;

  GST_DEBUG_OBJECT (self, "Generating %" GST_PTR_FORMAT
      " hash from file URI", memo->item);

  checksum = g_checksum_new (G_CHECKSUM_SHA256);

  if ((redirect_uri = clapper_media_item_get_redirect_uri (memo->item))) {
    g_checksum_update (checksum, (const guchar *) redirect_uri, -1);
    g_free (redirect_uri);
  } else {
    const gchar *uri = clapper_media_item_get_uri (memo->item);
    g_checksum_update (checksum, (const guchar *) uri, -1);
  }

  hash = g_strdup (g_checksum_get_string (checksum));
  g_checksum_free (checksum);

  return hash;
}

static gboolean
_memo_is_recallable (ClapperRecall *self, ClapperRecallMemo *memo)
{
  return (memo->position >= self->min_elapsed
      && memo->duration >= self->min_duration
      && memo->position <= memo->duration - self->min_remaining);
}

static void
_consider_playback_resume (ClapperRecall *self)
{
  if (self->resume_done
      || !self->current_memo->hash // if "hash" is set, "position" was restored too
      || self->current_memo->duration <= 0
      || self->state < CLAPPER_PLAYER_STATE_PAUSED)
    return;

  GST_DEBUG_OBJECT (self, "Considering whether to resume playback");

  if (self->auto_resume
      && self->current_memo->position > self->played_position // avoid seeking back
      && _memo_is_recallable (self, self->current_memo)) {
    ClapperPlayer *player = clapper_reactable_get_player (CLAPPER_REACTABLE_CAST (self));

    if (G_LIKELY (player != NULL)) {
      GST_INFO_OBJECT (self, "Resuming playback");
      clapper_player_seek (player, self->current_memo->position);
      gst_object_unref (player);
    }
  }

  self->resume_done = TRUE;
}

static void
timeline_insert_marker (ClapperRecall *self, ClapperRecallMemo *memo)
{
  ClapperTimeline *timeline = clapper_media_item_get_timeline (memo->item);

  GST_TRACE_OBJECT (self, "Insert marker into: %" GST_PTR_FORMAT, memo->item);
  clapper_timeline_insert_marker (timeline, memo->marker);
}

static void
timeline_remove_marker (ClapperRecall *self, ClapperRecallMemo *memo)
{
  ClapperTimeline *timeline = clapper_media_item_get_timeline (memo->item);

  GST_TRACE_OBJECT (self, "Remove marker from: %" GST_PTR_FORMAT, memo->item);
  clapper_timeline_remove_marker (timeline, memo->marker);
}

static void
_refresh_marker_presence (ClapperRecall *self, ClapperRecallMemo *memo, gboolean forced)
{
  gboolean has_marker, should_have_marker;

  GST_LOG_OBJECT (self, "Marker presence refresh for %" GST_PTR_FORMAT ", forced: %s",
      memo->item, forced ? "yes" : "no");

  has_marker = (memo->marker != NULL);
  should_have_marker = (self->mark_position && _memo_is_recallable (self, memo));

  if (forced || has_marker != should_have_marker) {
    if (has_marker) {
      timeline_remove_marker (self, memo);
      gst_clear_object (&memo->marker);
    }
    if (should_have_marker) {
      memo->marker = clapper_marker_new (RECALL_MARKER_TYPE,
          NULL, memo->position, memo->position);
      timeline_insert_marker (self, memo);
    }
  }
}

static void
_refresh_all_markers_presence (ClapperRecall *self)
{
  guint i;

  for (i = 0; i < self->memos->len; ++i) {
    ClapperRecallMemo *memo = (ClapperRecallMemo *) g_ptr_array_index (self->memos, i);
    _refresh_marker_presence (self, memo, FALSE);
  }
}

static gboolean
_on_hash_generated_cb (ClapperRecallData *data)
{
  ClapperRecall *self = data->recall;

  /* Move hash into memo, now that we are on enhancer thread */
  g_free (data->memo->hash);
  data->memo->hash = g_steal_pointer (&data->pending_hash);
  GST_LOG_OBJECT (self, "Hash filled for memo with %" GST_PTR_FORMAT,
      data->memo->item);

  /* Only read from DB if persistent storage is enabled and item did not play yet.
   * Otherwise if played before hash generation finished, keep that position value. */
  if (self->persistent_storage && data->memo->position <= 0 && _ensure_db (self)) {
    if (!self->recall_stmt) {
      const gchar *sql_cmd = "SELECT position FROM recall WHERE hash = ?;";
      sqlite3_prepare_v2 (self->db, sql_cmd, -1, &self->recall_stmt, NULL);
    }

    sqlite3_bind_text (self->recall_stmt, 1, data->memo->hash, -1, SQLITE_STATIC);

    /* NOTE: Only set if in DB, otherwise do NOT set to zero as this can
     * lead to inserting markers at zero for files that were never played */
    if (sqlite3_step (self->recall_stmt) == SQLITE_ROW)
      data->memo->position = CLAPPER_RECALL_USECONDS_TO_SECONDS (sqlite3_column_int64 (self->recall_stmt, 0));

    sqlite3_reset (self->recall_stmt);
    sqlite3_clear_bindings (self->recall_stmt);

    GST_INFO_OBJECT (self, "Recalled %" GST_PTR_FORMAT " position: %lf",
        data->memo->item, data->memo->position);
  }

  if (data->memo == self->current_memo)
    _consider_playback_resume (self);

  _refresh_marker_presence (self, data->memo, FALSE);

  return G_SOURCE_REMOVE;
}

static void
_memo_generate_hash_in_thread (ClapperRecallMemo *memo, ClapperRecall *self)
{
  ClapperRecallData *recall_data;
  gchar *hash;

  GST_DEBUG_OBJECT (self, "Generating hash for item: %" GST_PTR_FORMAT, memo->item);

  if (!(hash = _generate_data_hash (self, memo)))
    hash = _generate_uri_hash (self, memo); // Fallback that never fails

  GST_DEBUG_OBJECT (self, "Generated hash for item: %" GST_PTR_FORMAT ": %s",
      memo->item, hash);

  /* Thread pool does not call free func on successful run,
   * so we take it here without an additional ref */
  recall_data = clapper_recall_data_new_take (self, memo, hash);

  g_main_context_invoke_full (self->context, G_PRIORITY_DEFAULT,
      (GSourceFunc) _on_hash_generated_cb, recall_data,
      (GDestroyNotify) clapper_recall_data_free);
}

static gboolean
find_memo_by_item (ClapperRecall *self, ClapperMediaItem *search_item, guint *index)
{
  guint i;

  for (i = 0; i < self->memos->len; ++i) {
    ClapperRecallMemo *memo = (ClapperRecallMemo *) g_ptr_array_index (self->memos, i);

    if (search_item == memo->item) {
      if (index)
        *index = i;

      return TRUE;
    }
  }

  return FALSE;
}

static void
memorize_current_memo_position (ClapperRecall *self)
{
  GST_LOG_OBJECT (self, "Memorize");

  if (!self->persistent_storage
      || !self->current_memo->hash
      || self->current_memo->duration <= 0
      || !_ensure_db (self))
    return;

  if (!self->memorize_stmt) {
    const gchar *sql_cmd =
        "INSERT INTO recall (hash, position, updated) "
        "VALUES (?, ?, CURRENT_TIMESTAMP) "
        "ON CONFLICT(hash) DO UPDATE SET "
        "position = excluded.position,"
        "updated = CURRENT_TIMESTAMP;";
    sqlite3_prepare_v2 (self->db, sql_cmd, -1, &self->memorize_stmt, NULL);
  }

  /* NOTE: Since memo position has positive value, hash was set, thus accessing it here is safe */
  sqlite3_bind_text (self->memorize_stmt, 1, self->current_memo->hash, -1, SQLITE_STATIC);
  sqlite3_bind_int64 (self->memorize_stmt, 2, CLAPPER_RECALL_SECONDS_TO_USECONDS (self->current_memo->position));

  if (G_UNLIKELY (sqlite3_step (self->memorize_stmt) != SQLITE_DONE))
    GST_ERROR_OBJECT (self, "DB insert failed: %s", sqlite3_errmsg (self->db));

  sqlite3_reset (self->memorize_stmt);
  sqlite3_clear_bindings (self->memorize_stmt);

  GST_INFO_OBJECT (self, "%" GST_PTR_FORMAT " memorized position: %lf",
      self->current_memo->item, self->current_memo->position);
}

static void
clapper_recall_state_changed (ClapperReactable *reactable, ClapperPlayerState state)
{
  ClapperRecall *self = CLAPPER_RECALL_CAST (reactable);

  GST_DEBUG_OBJECT (self, "Playback status changed to: %u", state);
  self->state = state;

  /* NOTE: At paused state, duration might be still be unknown */
  if (self->current_memo)
    _consider_playback_resume (self);
}

static void
clapper_recall_position_changed (ClapperReactable *reactable, gdouble position)
{
  ClapperRecall *self = CLAPPER_RECALL_CAST (reactable);

  if (position == 0)
    return;

  GST_LOG_OBJECT (self, "Position changed to: %lf", position);
  self->played_position = position;
}

static void
clapper_recall_played_item_changed (ClapperReactable *reactable, ClapperMediaItem *item)
{
  ClapperRecall *self = CLAPPER_RECALL_CAST (reactable);
  guint index = 0;

  /* Memorize position of previous item */
  if (self->current_memo) {
    self->current_memo->position = self->played_position;
    memorize_current_memo_position (self);

    _refresh_marker_presence (self, self->current_memo, TRUE);
  }

  GST_DEBUG_OBJECT (self, "Played item changed to: %" GST_PTR_FORMAT, item);

  if (G_LIKELY (find_memo_by_item (self, item, &index)))
    self->current_memo = (ClapperRecallMemo *) g_ptr_array_index (self->memos, index);
  else
    self->current_memo = NULL;

  /* Reset */
  self->played_position = 0;
  self->resume_done = FALSE;

  /* Prioritize hash generation for played item */
  if (self->current_memo && !self->current_memo->hash) {
    if (g_thread_pool_move_to_front (self->pool, self->current_memo))
      GST_DEBUG_OBJECT (self, "Prioritized %" GST_PTR_FORMAT, self->current_memo->item);
    else // in middle of hash generation
      GST_LOG_OBJECT (self, "No need to prioritize %" GST_PTR_FORMAT, self->current_memo->item);
  }
}

static void
clapper_recall_item_updated (ClapperReactable *reactable, ClapperMediaItem *item, ClapperReactableItemUpdatedFlags flags)
{
  ClapperRecall *self = CLAPPER_RECALL_CAST (reactable);
  guint index = 0;

  if (!(flags & (CLAPPER_REACTABLE_ITEM_UPDATED_DURATION | CLAPPER_REACTABLE_ITEM_UPDATED_REDIRECT_URI)))
    return;

  if (G_LIKELY (find_memo_by_item (self, item, &index))) {
    ClapperRecallMemo *memo = (ClapperRecallMemo *) g_ptr_array_index (self->memos, index);

    if (flags & CLAPPER_REACTABLE_ITEM_UPDATED_REDIRECT_URI) {
      /* Clear hash and related to it position */
      g_clear_pointer (&memo->hash, g_free);
      memo->position = -1;

      GST_DEBUG_OBJECT (self, "%" GST_PTR_FORMAT " redirect URI updated",
          memo->item);

      /* Regenerate hash for new URI, prepend job if item is current. */
      g_thread_pool_push (self->pool, clapper_recall_memo_ref (memo), NULL);
      if (memo == self->current_memo)
        g_thread_pool_move_to_front (self->pool, self->current_memo);
    }
    if (flags & CLAPPER_REACTABLE_ITEM_UPDATED_DURATION) {
      memo->duration = clapper_media_item_get_duration (memo->item);
      GST_DEBUG_OBJECT (self, "%" GST_PTR_FORMAT " duration updated: %lf",
          memo->item, memo->duration);

      /* This checks hash presence, so it will not
       * resume if redirect URI was changed too */
      if (memo == self->current_memo)
        _consider_playback_resume (self);
    }

    /* Needs to be done for either redirect or duration update */
    _refresh_marker_presence (self, memo, FALSE);
  }
}

static void
clapper_recall_queue_item_added (ClapperReactable *reactable, ClapperMediaItem *item, guint index)
{
  ClapperRecall *self = CLAPPER_RECALL_CAST (reactable);
  ClapperRecallMemo *memo;

  GST_DEBUG_OBJECT (self, "Queue %" GST_PTR_FORMAT " added", item);

  memo = clapper_recall_memo_new_for_item (item);

  g_ptr_array_insert (self->memos, index, clapper_recall_memo_ref (memo));
  g_thread_pool_push (self->pool, memo, NULL);
}

static void
clapper_recall_queue_item_removed (ClapperReactable *reactable, ClapperMediaItem *item, guint index)
{
  ClapperRecall *self = CLAPPER_RECALL_CAST (reactable);
  ClapperRecallMemo *memo;

  GST_DEBUG_OBJECT (self, "Queue item removed at position: %u", index);

  memo = (ClapperRecallMemo *) g_ptr_array_steal_index (self->memos, index);

  if (memo == self->current_memo) {
    self->current_memo->position = self->played_position;
    memorize_current_memo_position (self);

    self->current_memo = NULL;
  }

  if (memo->marker)
    timeline_remove_marker (self, memo);

  clapper_recall_memo_unref (memo);
}

static void
clapper_recall_queue_item_repositioned (ClapperReactable *reactable, guint before, guint after)
{
  ClapperRecall *self = CLAPPER_RECALL_CAST (reactable);
  ClapperRecallMemo *memo;

  GST_DEBUG_OBJECT (self, "Queue item repositioned: %u -> %u", before, after);

  memo = (ClapperRecallMemo *) g_ptr_array_steal_index (self->memos, before);
  g_ptr_array_insert (self->memos, after, memo);
}

static void
clapper_recall_queue_cleared (ClapperReactable *reactable)
{
  ClapperRecall *self = CLAPPER_RECALL_CAST (reactable);
  guint i;

  if (self->current_memo) {
    self->current_memo->position = self->played_position;
    memorize_current_memo_position (self);

    self->current_memo = NULL;
  }

  i = self->memos->len;
  while (i-- > 0) {
    ClapperRecallMemo *memo = (ClapperRecallMemo *) g_ptr_array_steal_index (self->memos, i);

    if (memo->marker)
      timeline_remove_marker (self, memo);

    clapper_recall_memo_unref (memo);
  }
}

static void
clapper_recall_reactable_iface_init (ClapperReactableInterface *iface)
{
  iface->state_changed = clapper_recall_state_changed;
  iface->position_changed = clapper_recall_position_changed;
  iface->played_item_changed = clapper_recall_played_item_changed;
  iface->item_updated = clapper_recall_item_updated;
  iface->queue_item_added = clapper_recall_queue_item_added;
  iface->queue_item_removed = clapper_recall_queue_item_removed;
  iface->queue_item_repositioned = clapper_recall_queue_item_repositioned;
  iface->queue_cleared = clapper_recall_queue_cleared;
}

#define parent_class clapper_recall_parent_class
G_DEFINE_TYPE_WITH_CODE (ClapperRecall, clapper_recall, GST_TYPE_OBJECT,
    G_IMPLEMENT_INTERFACE (CLAPPER_TYPE_REACTABLE, clapper_recall_reactable_iface_init));

static void
clapper_recall_init (ClapperRecall *self)
{
  self->context = g_main_context_get_thread_default ();

  self->pool = g_thread_pool_new_full ((GFunc) _memo_generate_hash_in_thread,
      self, (GDestroyNotify) clapper_recall_memo_unref,
      MAX (MIN (2, g_get_num_processors ()), 1), // 2 threads should be enough
      FALSE, NULL);

  self->memos = g_ptr_array_new_with_free_func ((GDestroyNotify) clapper_recall_memo_unref);

  self->persistent_storage = DEFAULT_PERSISTENT_STORAGE;
  self->mark_position = DEFAULT_MARK_POSITION;
  self->auto_resume = DEFAULT_AUTO_RESUME;
  self->min_duration = DEFAULT_MIN_DURATION;
  self->min_elapsed = DEFAULT_MIN_ELAPSED;
  self->min_remaining = DEFAULT_MIN_REMAINING;
}

static void
clapper_recall_dispose (GObject *object)
{
  ClapperRecall *self = CLAPPER_RECALL_CAST (object);

  if (self->pool) {
    g_thread_pool_free (self->pool, TRUE, FALSE);
    self->pool = NULL;
  }

  /* Memorize before cleanup */
  if (self->current_memo) {
    self->current_memo->position = self->played_position;
    memorize_current_memo_position (self);

    self->current_memo = NULL;
  }
  g_clear_pointer (&self->memos, g_ptr_array_unref);

  if (self->db) {
    _clean_db (self);
    _truncate_db (self);
    sqlite3_close (self->db);
    self->db = NULL;
  }

  G_OBJECT_CLASS (parent_class)->dispose (object);
}

static void
clapper_recall_finalize (GObject *object)
{
  ClapperRecall *self = CLAPPER_RECALL_CAST (object);

  GST_TRACE_OBJECT (self, "Finalize");

  if (self->memorize_stmt)
    sqlite3_finalize (self->memorize_stmt);
  if (self->recall_stmt)
    sqlite3_finalize (self->recall_stmt);

  G_OBJECT_CLASS (parent_class)->finalize (object);
}

static void
clapper_recall_set_mark_position (ClapperRecall *self, gboolean value)
{
  if (self->mark_position != value) {
    self->mark_position = value;
    _refresh_all_markers_presence (self);
  }
}

static void
clapper_recall_set_time_ptr (ClapperRecall *self, gint *time_ptr, gint value)
{
  if (*time_ptr != value) {
    *time_ptr = value;
    _refresh_all_markers_presence (self);
  }
}

static void
clapper_recall_set_property (GObject *object, guint prop_id,
    const GValue *value, GParamSpec *pspec)
{
  ClapperRecall *self = CLAPPER_RECALL_CAST (object);

  switch (prop_id) {
    case PROP_PERSISTENT_STORAGE:
      self->persistent_storage = g_value_get_boolean (value);
      break;
    case PROP_MARK_POSITION:
      clapper_recall_set_mark_position (self, g_value_get_boolean (value));
      break;
    case PROP_AUTO_RESUME:
      self->auto_resume = g_value_get_boolean (value);
      break;
    case PROP_MIN_DURATION:
      clapper_recall_set_time_ptr (self, &self->min_duration, g_value_get_int (value));
      break;
    case PROP_MIN_ELAPSED:
      clapper_recall_set_time_ptr (self, &self->min_elapsed, g_value_get_int (value));
      break;
    case PROP_MIN_REMAINING:
      clapper_recall_set_time_ptr (self, &self->min_remaining, g_value_get_int (value));
      break;
    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
      break;
  }
}

static void
clapper_recall_get_property (GObject *object, guint prop_id,
    GValue *value, GParamSpec *pspec)
{
  ClapperRecall *self = CLAPPER_RECALL_CAST (object);

  switch (prop_id) {
    case PROP_PERSISTENT_STORAGE:
      g_value_set_boolean (value, self->persistent_storage);
      break;
    case PROP_MARK_POSITION:
      g_value_set_boolean (value, self->mark_position);
      break;
    case PROP_AUTO_RESUME:
      g_value_set_boolean (value, self->auto_resume);
      break;
    case PROP_MIN_DURATION:
      g_value_set_int (value, self->min_duration);
      break;
    case PROP_MIN_ELAPSED:
      g_value_set_int (value, self->min_elapsed);
      break;
    case PROP_MIN_REMAINING:
      g_value_set_int (value, self->min_remaining);
      break;
    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
      break;
  }
}

static void
clapper_recall_class_init (ClapperRecallClass *klass)
{
  GObjectClass *gobject_class = (GObjectClass *) klass;

  GST_DEBUG_CATEGORY_INIT (GST_CAT_DEFAULT, "clapperrecall", 0,
      "Clapper Recall");

  gobject_class->get_property = clapper_recall_get_property;
  gobject_class->set_property = clapper_recall_set_property;
  gobject_class->dispose = clapper_recall_dispose;
  gobject_class->finalize = clapper_recall_finalize;

  /**
   * ClapperRecall:persistent-storage:
   *
   * Store media playback positions in a local database,
   * so recall functionality can be persistent.
   */
  param_specs[PROP_PERSISTENT_STORAGE] = g_param_spec_boolean ("persistent-storage",
      "Persistent Storage", "Store media playback positions in a local database,"
      " so recall functionality can be persistent", DEFAULT_PERSISTENT_STORAGE,
      G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS | CLAPPER_ENHANCER_PARAM_GLOBAL | CLAPPER_ENHANCER_PARAM_LOCAL);

  /**
   * ClapperRecall:mark-position:
   *
   * Insert a marker with remembered position into timeline.
   *
   * Applications that handle Clapper markers will show it
   * among any other eventual markers a media item might have.
   */
  param_specs[PROP_MARK_POSITION] = g_param_spec_boolean ("mark-position",
      "Mark Position", "Insert a marker with remembered position into timeline", DEFAULT_MARK_POSITION,
      G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS | CLAPPER_ENHANCER_PARAM_GLOBAL | CLAPPER_ENHANCER_PARAM_LOCAL);

  /**
   * ClapperRecall:auto-resume:
   *
   * Automatically seek to the last remembered position.
   */
  param_specs[PROP_AUTO_RESUME] = g_param_spec_boolean ("auto-resume",
      "Auto Resume", "Automatically seek to the last remembered position", DEFAULT_AUTO_RESUME,
      G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS | CLAPPER_ENHANCER_PARAM_GLOBAL);

  /**
   * ClapperRecall:min-duration:
   *
   * Recall playback position if media duration is at least this many seconds.
   */
  param_specs[PROP_MIN_DURATION] = g_param_spec_int ("min-duration",
      "Minimum Duration", "Recall playback position if media duration is at least this many seconds",
      1, G_MAXINT, DEFAULT_MIN_DURATION,
      G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS | CLAPPER_ENHANCER_PARAM_GLOBAL);

  /**
   * ClapperRecall:min-elapsed:
   *
   * Recall playback position if media played for at least this many seconds.
   */
  param_specs[PROP_MIN_ELAPSED] = g_param_spec_int ("min-elapsed",
      "Minimum Elapsed", "Recall playback position if media played for at least this many seconds",
      0, G_MAXINT, DEFAULT_MIN_ELAPSED,
      G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS | CLAPPER_ENHANCER_PARAM_GLOBAL);

  /**
   * ClapperRecall:min-remaining:
   *
   * Recall playback position if at least this many seconds remained.
   */
  param_specs[PROP_MIN_REMAINING] = g_param_spec_int ("min-remaining",
      "Minimum Remaining", "Recall playback position if at least this many seconds remained",
      0, G_MAXINT, DEFAULT_MIN_REMAINING,
      G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS | CLAPPER_ENHANCER_PARAM_GLOBAL);

  g_object_class_install_properties (gobject_class, PROP_LAST, param_specs);
}

void
peas_register_types (PeasObjectModule *module)
{
  peas_object_module_register_extension_type (module, CLAPPER_TYPE_REACTABLE, CLAPPER_TYPE_RECALL);
}
