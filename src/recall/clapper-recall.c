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

#include <glib.h>
#include <glib-object.h>
#include <gio/gio.h>
#include <gmodule.h>
#include <libpeas.h>
#include <clapper/clapper.h>

#include <sqlite3.h>

#define DEFAULT_MARK_POSITION FALSE

#define CLAPPER_RECALL_SECONDS_TO_USECONDS(seconds) ((gint64) (seconds * G_GINT64_CONSTANT (1000000)))
#define CLAPPER_RECALL_USECONDS_TO_SECONDS(useconds) ((gdouble) useconds / G_GINT64_CONSTANT (1000000))

#define RECALL_MARKER_TYPE (CLAPPER_MARKER_TYPE_CUSTOM_3)
#define RECALL_MARKER_NAME "recall-marker"
#define CHUNK_SIZE 4096

#define GST_CAT_DEFAULT clapper_recall_debug
GST_DEBUG_CATEGORY_STATIC (GST_CAT_DEFAULT);

#define CLAPPER_TYPE_RECALL (clapper_recall_get_type())
#define CLAPPER_RECALL_CAST(obj) ((ClapperRecall *)(obj))
G_DECLARE_FINAL_TYPE (ClapperRecall, clapper_recall, CLAPPER, RECALL, GstObject);

G_MODULE_EXPORT void peas_register_types (PeasObjectModule *module);

struct _ClapperRecall
{
  GstObject parent;

  sqlite3 *db;
  gboolean db_ensured;

  sqlite3_stmt *memorize_stmt;
  sqlite3_stmt *recall_stmt;

  guint updates_counter;
  gboolean playing;

  ClapperMediaItem *played_item;
  gchar *hash;
  gdouble position;

  gboolean mark_position;
};

enum
{
  PROP_0,
  PROP_MARK_POSITION,
  PROP_LAST
};

static GParamSpec *param_specs[PROP_LAST] = { NULL, };

static inline gchar *
_make_db_filename (ClapperRecall *self)
{
  GFile *cache_dir;
  GError *error = NULL;
  gchar *cache_dir_path, *db_filename;

  cache_dir = g_file_new_build_filename (g_get_user_cache_dir (),
      "clapper-0.0", "enhancers", "clapper-recall", NULL);

  if (!g_file_make_directory_with_parents (cache_dir, NULL, &error)) {
    if (error->domain != G_IO_ERROR || error->code != G_IO_ERROR_EXISTS) {
      GST_ERROR_OBJECT (self, "Failed to create directory for DB: %s", error->message);
      g_clear_object (&cache_dir);
    }
    g_error_free (error);
  }

  if (!cache_dir)
    return NULL;

  cache_dir_path = g_file_get_path (cache_dir);
  g_object_unref (cache_dir);

  db_filename = g_build_filename (cache_dir_path, "recall.db", NULL);
  g_free (cache_dir_path);

  return db_filename;
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
        const gchar *create_table =
            "CREATE TABLE IF NOT EXISTS recall ("
            "id INTEGER PRIMARY KEY AUTOINCREMENT,"
            "hash TEXT UNIQUE,"
            "position INTEGER"
            ");";

        if ((had_error = sqlite3_exec (self->db, create_table, NULL, NULL, &errmsg) != SQLITE_OK)) {
          GST_ERROR_OBJECT (self, "Failed to create table: %s", errmsg);
          sqlite3_free (errmsg);
        }
      } else {
        GST_ERROR_OBJECT (self, "Failed to create table: %s", sqlite3_errmsg (self->db));
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

static gboolean
_ensure_hash (ClapperRecall *self)
{
  if (self->hash)
    return TRUE;

  if (!self->hash) {
    /* FIXME: Try to generate from file data as first method */
  }
  if (!self->hash) {
    GChecksum *checksum = g_checksum_new (G_CHECKSUM_SHA256);
    const gchar *uri = clapper_media_item_get_uri (self->played_item);

    g_checksum_update (checksum, (const guchar *) uri, -1);

    self->hash = g_strdup (g_checksum_get_string (checksum));
    g_checksum_free (checksum);
  }

  GST_DEBUG_OBJECT (self, "Generated %" GST_PTR_FORMAT " hash: %s",
      self->played_item, GST_STR_NULL (self->hash));

  return (self->hash != NULL);
}

static void
memorize_position (ClapperRecall *self)
{
  GST_LOG_OBJECT (self, "Memorize");

  if (!self->played_item || !_ensure_db (self) || !_ensure_hash (self))
    return;

  if (!self->memorize_stmt) {
    const gchar *sql_cmd =
        "INSERT INTO recall (hash, position) "
        "VALUES (?, ?) "
        "ON CONFLICT(hash) DO UPDATE SET position=excluded.position;";
    sqlite3_prepare_v2 (self->db, sql_cmd, -1, &self->memorize_stmt, NULL);
  }

  sqlite3_bind_text (self->memorize_stmt, 1, self->hash, -1, SQLITE_STATIC);
  sqlite3_bind_int64 (self->memorize_stmt, 2, CLAPPER_RECALL_SECONDS_TO_USECONDS (self->position));

  if (G_UNLIKELY (sqlite3_step (self->memorize_stmt) != SQLITE_DONE))
    GST_ERROR_OBJECT (self, "DB insert failed: %s", sqlite3_errmsg (self->db));

  sqlite3_reset (self->memorize_stmt);
  sqlite3_clear_bindings (self->memorize_stmt);

  /* Truncate if accumulated many changes */
  /* FIXME: Run on idle */
  if ((++self->updates_counter % 100) == 0) {
    _truncate_db (self);
    self->updates_counter = 0;
  }

  GST_INFO_OBJECT (self, "Item memorized position: %lf", self->position);
}

static gdouble
recall_position (ClapperRecall *self)
{
  gdouble position = 0;

  GST_LOG_OBJECT (self, "Recall");

  if (!self->played_item || !_ensure_db (self) || !_ensure_hash (self))
    return 0;

  if (!self->recall_stmt) {
    const gchar *sql_cmd = "SELECT position FROM recall WHERE hash = ?;";
    sqlite3_prepare_v2 (self->db, sql_cmd, -1, &self->recall_stmt, NULL);
  }

  sqlite3_bind_text (self->recall_stmt, 1, self->hash, -1, SQLITE_STATIC);

  if (sqlite3_step (self->recall_stmt) == SQLITE_ROW)
    position = CLAPPER_RECALL_USECONDS_TO_SECONDS (sqlite3_column_int64 (self->recall_stmt, 0));

  sqlite3_reset (self->recall_stmt);
  sqlite3_clear_bindings (self->recall_stmt);

  GST_INFO_OBJECT (self, "Item recalled position: %lf", position);

  return position;
}

static void
clapper_recall_state_changed (ClapperReactable *reactable, ClapperPlayerState state)
{
  ClapperRecall *self = CLAPPER_RECALL_CAST (reactable);

  GST_DEBUG_OBJECT (self, "Playback status changed to: %u", state);
  self->playing = (state == CLAPPER_PLAYER_STATE_PLAYING);
}

static void
clapper_recall_position_changed (ClapperReactable *reactable, gdouble position)
{
  ClapperRecall *self = CLAPPER_RECALL_CAST (reactable);

  if (position == 0)
    return;

  GST_LOG_OBJECT (self, "Position changed to: %lf", position);
  self->position = position;
}

static void
clapper_recall_played_item_changed (ClapperReactable *reactable, ClapperMediaItem *item)
{
  ClapperRecall *self = CLAPPER_RECALL_CAST (reactable);
  gdouble recalled_position;

  /* Memorize exact position of last item */
  if (self->played_item)
    memorize_position (self);

  GST_DEBUG_OBJECT (self, "Played item changed to: %" GST_PTR_FORMAT, item);
  gst_object_replace ((GstObject **) &self->played_item, GST_OBJECT_CAST (item));

  /* Reset */
  g_clear_pointer (&self->hash, g_free);
  self->position = 0;

  recalled_position = recall_position (self);

  if (recalled_position > 0) { // FIXME: > min_allowed
    ClapperTimeline *timeline;
    ClapperMarker *recall_marker;
    guint i, n_markers;

    timeline = clapper_media_item_get_timeline (self->played_item);
    n_markers = clapper_timeline_get_n_markers (timeline);

    GST_ERROR ("N MARKERS: %u", n_markers);

    /* Find and remove old custom marker */
    for (i = 0; i < n_markers; ++i) {
      ClapperMarker *marker = clapper_timeline_get_marker (timeline, i);

      GST_ERROR ("NAME: %s", GST_OBJECT_NAME (marker));

      if (clapper_marker_get_marker_type (marker) == RECALL_MARKER_TYPE) {
        gboolean found = FALSE;

        /* Type is not enough to be sure, also check custom name */
        GST_OBJECT_LOCK (marker);
        found = (strcmp (GST_OBJECT_NAME (marker), RECALL_MARKER_NAME) == 0);
        GST_OBJECT_UNLOCK (marker);

        if (found) {
          clapper_timeline_remove_marker (timeline, marker);
          gst_object_unref (marker);

          GST_ERROR ("FOUND!");

          break;
        }
      }

      gst_object_unref (marker);
    }

    /* Add new marker with current position */
    recall_marker = clapper_marker_new (RECALL_MARKER_TYPE,
        "Resume point", recalled_position, recalled_position);
    gst_object_set_name (GST_OBJECT_CAST (recall_marker), RECALL_MARKER_NAME);

    clapper_timeline_insert_marker (timeline, recall_marker);
    gst_object_unref (recall_marker);
  }
}

static void
clapper_recall_reactable_iface_init (ClapperReactableInterface *iface)
{
  iface->state_changed = clapper_recall_state_changed;
  iface->position_changed = clapper_recall_position_changed;
  iface->played_item_changed = clapper_recall_played_item_changed;
}

#define parent_class clapper_recall_parent_class
G_DEFINE_TYPE_WITH_CODE (ClapperRecall, clapper_recall, GST_TYPE_OBJECT,
    G_IMPLEMENT_INTERFACE (CLAPPER_TYPE_REACTABLE, clapper_recall_reactable_iface_init));

static void
clapper_recall_set_mark_position (ClapperRecall *self, gboolean enabled)
{
  if (self->mark_position == enabled)
    return;

  self->mark_position = enabled;
}

static void
clapper_recall_init (ClapperRecall *self)
{
  self->mark_position = DEFAULT_MARK_POSITION;
}

static void
clapper_recall_dispose (GObject *object)
{
  ClapperRecall *self = CLAPPER_RECALL_CAST (object);

  /* Memorize before cleanup */
  if (self->played_item) {
    memorize_position (self);
    gst_clear_object (&self->played_item);
  }

  G_OBJECT_CLASS (parent_class)->dispose (object);
}

static void
clapper_recall_finalize (GObject *object)
{
  ClapperRecall *self = CLAPPER_RECALL_CAST (object);

  GST_TRACE_OBJECT (self, "Finalize");

  g_free (self->hash);

  if (self->db) {
    _truncate_db (self);
    sqlite3_close (self->db);
  }
  if (self->memorize_stmt)
    sqlite3_finalize (self->memorize_stmt);
  if (self->recall_stmt)
    sqlite3_finalize (self->recall_stmt);

  G_OBJECT_CLASS (parent_class)->finalize (object);
}

static void
clapper_recall_set_property (GObject *object, guint prop_id,
    const GValue *value, GParamSpec *pspec)
{
  ClapperRecall *self = CLAPPER_RECALL_CAST (object);

  switch (prop_id) {
    case PROP_MARK_POSITION:
      clapper_recall_set_mark_position (self, g_value_get_boolean (value));
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
    case PROP_MARK_POSITION:
      g_value_set_boolean (value, self->mark_position);
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
   * ClapperRecall:mark-position:
   *
   * Whether to insert a marker with remembered position into timeline.
   *
   * Applications that handle Clapper markers will show it
   * among any other eventual markers a media item might have.
   */
  param_specs[PROP_MARK_POSITION] = g_param_spec_boolean ("mark-position",
      "Mark Position", "Whether to insert a marker with remembered position into timeline", DEFAULT_MARK_POSITION,
      G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS | CLAPPER_ENHANCER_PARAM_GLOBAL | CLAPPER_ENHANCER_PARAM_LOCAL);

  g_object_class_install_properties (gobject_class, PROP_LAST, param_specs);
}

void
peas_register_types (PeasObjectModule *module)
{
  peas_object_module_register_extension_type (module, CLAPPER_TYPE_REACTABLE, CLAPPER_TYPE_RECALL);
}
