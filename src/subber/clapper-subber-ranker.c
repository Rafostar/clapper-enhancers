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

#include <gst/gst.h>

#include "clapper-subber-ranker.h"

#define SCORE_HASH_MATCH         20000
#define SCORE_STRING_MATCH       10000
#define SCORE_TRUSTED_UPLOADER    3000
#define SCORE_UPLOADER_RANK       2500
#define SCORE_DOWNLOAD_COUNT      2000
#define SCORE_RECENT_UPLOAD       1000

#define MAX_LEVENSHTEIN_LEN 192

#define GST_CAT_DEFAULT clapper_subber_ranker_debug
GST_DEBUG_CATEGORY_STATIC (GST_CAT_DEFAULT);

struct LevenshteinData
{
  const gchar *s;
  const gchar *t;
  gint ls;
  gint lt;
  gint **d;
};

void
clapper_subber_ranker_debug_init (void)
{
  GST_DEBUG_CATEGORY_INIT (GST_CAT_DEFAULT, "clappersubberranker",
      GST_DEBUG_FG_BLUE, "Clapper Subber Ranker");
}

/*
 * Levenshtein distance algorithm, source:
 * https://rosettacode.org/wiki/Levenshtein_distance#C
 */
static gint
_lev_dist (struct LevenshteinData *data, gint i, gint j)
{
  gint x;

  if (data->d[i][j] >= 0)
    return data->d[i][j];

  if (i == data->ls) {
    x = data->lt - j;
  } else if (j == data->lt) {
    x = data->ls - i;
  } else if (data->s[i] == data->t[j]) {
    x = _lev_dist (data, i + 1, j + 1);
  } else {
    gint y;

    x = _lev_dist (data, i + 1, j + 1);
    if ((y = _lev_dist (data, i, j + 1)) < x) x = y;
    if ((y = _lev_dist (data, i + 1, j)) < x) x = y;
    x++;
  }

  return data->d[i][j] = x;
}

static inline gint
_levenshtein (const gchar *s, gint ls, const gchar *t, gint lt)
{
  gint i, j, **d, *pool, result;
  struct LevenshteinData data = { s, t, ls, lt, NULL };

  d = g_new (gint *, ls + 1);
  pool = g_new (gint, (ls + 1) * (lt + 1));
  for (i = 0; i <= ls; ++i) {
    d[i] = pool + i * (lt + 1);
    for (j = 0; j <= lt; ++j)
      d[i][j] = -1;
  }

  data.d = d;
  result = _lev_dist (&data, 0, 0);

  g_free (d);
  g_free (pool);

  return result;
}

static inline gfloat
similarity (const gchar *lower_1, const gchar *lower_2)
{
  gint len_1 = strlen (lower_1);
  gint len_2 = strlen (lower_2);
  gint res;

  /* Too long to calculate efficiently */
  if (G_UNLIKELY (len_1 > MAX_LEVENSHTEIN_LEN
      || len_2 > MAX_LEVENSHTEIN_LEN))
    return 0;

  res = _levenshtein (lower_1, len_1, lower_2, len_2);
  return (1.0 - ((gfloat) (res) / MAX (len_1, len_2)));
}

static guint
_hash_score (JsonReader *reader)
{
  guint score = 0;

  if (json_utils_get_boolean (reader, "moviehash_match", NULL))
    score = SCORE_HASH_MATCH;

  GST_LOG ("Hash match score: %u", score);
  return score;
}

static guint
_string_score (JsonReader *reader, const gchar *name_lower, const gchar *json_key)
{
  const gchar *release;
  guint score = 0;

  if ((release = json_utils_get_string (reader, json_key, NULL))) {
    gchar *release_lower = g_utf8_strdown (release, -1);
    gfloat result = similarity (release_lower, name_lower);
    score = result * SCORE_STRING_MATCH;
    g_free (release_lower);
  }

  GST_LOG ("String (%s) match score: %u", json_key, score);
  return score;
}

static guint
_trusted_score (JsonReader *reader)
{
  guint score = 0;

  if (json_utils_get_boolean (reader, "from_trusted", NULL))
    score = SCORE_TRUSTED_UPLOADER;

  GST_LOG ("Trusted source score: %u", score);
  return score;
}

static guint
_uploader_rank_score (JsonReader *reader)
{
  const gchar *rank;
  guint score = 0;

  if ((rank = json_utils_get_string (reader, "uploader", "rank", NULL))) {
    if (g_ascii_strcasecmp (rank, "Gold member") == 0
        || g_ascii_strcasecmp (rank, "Application Developers") == 0)
      score = SCORE_UPLOADER_RANK;
  }

  GST_LOG ("Uploader rank score: %u", score);
  return score;
}

static guint
_downloads_score (JsonReader *reader)
{
  gint64 downloads = json_utils_get_int (reader, "download_count", NULL);
  guint score = MIN (downloads / 100, SCORE_DOWNLOAD_COUNT);

  GST_LOG ("Downloads number score: %u", score);
  return score;
}

static inline gint64
_days_till_now (const gchar *date_str)
{
  GDateTime *upload, *now;
  GTimeSpan diff;

  upload = g_date_time_new_from_iso8601 (date_str, NULL);
  if (!upload)
    return -1;

  now = g_date_time_new_now_utc ();
  diff = g_date_time_difference (now, upload);

  g_date_time_unref (upload);
  g_date_time_unref (now);

  return (gint64) (diff / G_TIME_SPAN_DAY); // Convert to days
}

static guint
_recent_upload_score (JsonReader *reader)
{
  const gchar *date_str;
  guint score = 0;

  if ((date_str = json_utils_get_string (reader, "upload_date", NULL))) {
    gint64 days = _days_till_now (date_str);
    if (days >= 0) {
      gdouble factor = 1.0 / (1.0 + (days / 365.0));
      score = SCORE_RECENT_UPLOAD * factor;
    }
  }

  GST_LOG ("Recent upload score: %u", score);
  return score;
}

static guint
calc_entry_score (JsonReader *reader, const gchar *name_lower)
{
  const gchar *subs_id;
  guint score = 0;

  subs_id = json_utils_get_string (reader, "subtitle_id", NULL);
  GST_DEBUG ("Calculating entry ID \"%s\" score...", subs_id);

  score += _hash_score (reader);
  score += _string_score (reader, name_lower, "release");
  score += _trusted_score (reader);
  score += _uploader_rank_score (reader);
  score += _downloads_score (reader);
  score += _recent_upload_score (reader);

  GST_DEBUG ("Entry score: %u", score);

  return score;
}

/*
 * Each OpenSubtitles data entry provides an array of files with one or more
 * subtitle file (for cases such as multiple discs). For this reason we run
 * similarity test again among all these files to find the correct one.
 */
static gint64
select_file_id (JsonReader *reader, const gchar *name_lower, GCancellable *cancellable)
{
  gint64 best_id = 0;
  guint best_score = 0;

  if (json_utils_go_to (reader, "attributes", "files", NULL)) {
    gint i, count = json_utils_count_elements (reader, NULL);

    for (i = 0; i < count; ++i) {
      if (G_UNLIKELY (g_cancellable_is_cancelled (cancellable)))
        break;
      if (json_utils_go_to (reader, JSON_UTILS_ARRAY_INDEX (i), NULL)) {
        guint file_score = _string_score (reader, name_lower, "file_name");

        if (file_score > best_score) {
          gint64 selected_id = json_utils_get_int (reader, "file_id", NULL);

          if (selected_id > 0) {
            best_id = selected_id;
            best_score = file_score;
          }
        }
        json_utils_go_back (reader, 1);
      }
    }
    json_utils_go_back (reader, 2);
  }

  return (!g_cancellable_is_cancelled (cancellable)) ? best_id : 0;
}

gint64
clapper_subber_ranker_choose_file_id (JsonReader *reader, const gchar *file_name,
    GCancellable *cancellable, GError **error)
{
  gint64 best_id = 0;
  guint best_score = 0;

  if (json_utils_go_to (reader, "data", NULL)) {
    gint count, best_index = -1;

    count = json_utils_count_elements (reader, NULL);
    GST_DEBUG ("Found entries: %i", count);

    if (count > 0) {
      gchar *name_lower = g_utf8_strdown (file_name, -1);
      gint i, n_searches = MIN (count, 50); // limit search

      for (i = 0; i < n_searches; ++i) {
        if (G_UNLIKELY (g_cancellable_is_cancelled (cancellable)))
          break;
        if (json_utils_go_to (reader, JSON_UTILS_ARRAY_INDEX (i), NULL)) {
          const gchar *type = json_utils_get_string (reader, "type", NULL);

          if (g_strcmp0 (type, "subtitle") == 0
              && json_utils_go_to (reader, "attributes", NULL)) {
            guint entry_score = calc_entry_score (reader, name_lower);

            if (entry_score > best_score) {
              best_index = i;
              best_score = entry_score;
            }
            json_utils_go_back (reader, 1);
          }
          json_utils_go_back (reader, 1);
        }
      }

      if (best_index >= 0 && !g_cancellable_is_cancelled (cancellable)
          && json_utils_go_to (reader, JSON_UTILS_ARRAY_INDEX (best_index), NULL)) {
        best_id = select_file_id (reader, name_lower, cancellable);
        json_utils_go_back (reader, 1);
      }

      g_free (name_lower);
    }
    json_utils_go_back (reader, 1);
  }

  if (best_id > 0) {
    GST_INFO ("Selected subtitles file ID: %"
        G_GINT64_FORMAT " (score: %u)", best_id, best_score);
  } else {
    g_set_error (error, G_IO_ERROR, G_IO_ERROR_FAILED,
        (g_cancellable_is_cancelled (cancellable))
        ? "Subtitles search was cancelled"
        : "No subtitles found");
  }

  return best_id;
}
