# Clapper Enhancer yt-dlp
# Copyright (C) 2025 Rafał Dzięgiel <rafostar.github@gmail.com>
#
# This library is free software; you can redistribute it and/or
# modify it under the terms of the GNU Lesser General Public
# License as published by the Free Software Foundation; either
# version 2.1 of the License, or (at your option) any later version.
#
# This library is distributed in the hope that it will be useful,
# but WITHOUT ANY WARRANTY; without even the implied warranty of
# MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the GNU
# Lesser General Public License for more details.
#
# You should have received a copy of the GNU Lesser General Public
# License along with this library; if not, see
# <https://www.gnu.org/licenses/>.

import os, json, gi
gi.require_version('GLib', '2.0')
gi.require_version('GObject', '2.0')
gi.require_version('Gio', '2.0')
gi.require_version('Gst', '1.0')
gi.require_version('GstTag', '1.0')
gi.require_version('Clapper', '0.0')
from gi.repository import GLib, GObject, Gio, Gst, GstTag, Clapper

import clapper_yt_dlp_debug as debug

def _find_best_thumbnail(thumbnails):
    best = None
    best_area = 0

    for t in thumbnails:
        width = t.get('width') or 0
        height = t.get('height') or 0

        if (area := width * height) > best_area and (url := t.get('url')):
            best = t
            best_area = area

    return best

def _fetch_image_sample(thumbnails, cancellable: Gio.Cancellable):
    if not (best := _find_best_thumbnail(thumbnails)):
        debug.print_leveled(Gst.DebugLevel.INFO, 'No preview image found')
        return None

    debug.print_leveled(Gst.DebugLevel.DEBUG, 'Fetching image data...')

    file = Gio.File.new_for_uri(best['url'])
    gbytes = None

    try:
        gbytes, _ = file.load_bytes(cancellable)
    except GLib.Error as e:
        # Just print, continue extraction without thumbnail
        debug.print_leveled(Gst.DebugLevel.ERROR, f'Thumbnail fetch failed: {e.message}')
        return None

    if gbytes.get_size() == 0:
        debug.print_leveled(Gst.DebugLevel.ERROR, 'Fetched thumbnail is empty')
        return None

    debug.print_leveled(Gst.DebugLevel.DEBUG, 'Fetched image data')

    # According to GStreamer docs, for preview images "NONE" image type should be used
    sample = GstTag.tag_image_data_to_image_sample(gbytes.get_data(), GstTag.TagImageType.NONE)
    if not sample:
        debug.print_leveled(Gst.DebugLevel.ERROR, 'Could not generate image sample from data')

    return sample

def harvest_add_item_data(harvest: Clapper.Harvest, info, cancellable: Gio.Cancellable):
    # Add tags
    if (val := info.get('title')):
        harvest.tags_add(Gst.TAG_TITLE, val)
    if (val := info.get('duration')):
        value = GObject.Value()
        value.init(GObject.TYPE_UINT64)
        value.set_uint64(val * Gst.SECOND)
        harvest.tags_add(Gst.TAG_DURATION, value)
    if (val := info.get('channel')):
        harvest.tags_add(Gst.TAG_ARTIST, val)
    if (val := info.get('description')):
        harvest.tags_add(Gst.TAG_DESCRIPTION, val)
    if (cats := info.get('categories')):
        [harvest.tags_add(Gst.TAG_GENRE, val) for val in cats]
    if (th := info.get('thumbnails')) and (val := _fetch_image_sample(th, cancellable)):
        harvest.tags_add(Gst.TAG_PREVIEW_IMAGE, val)

    # Add TOC
    if (val := info.get('chapters')):
        for index, chap in enumerate(val):
            title, start, end = chap.get('title'), chap.get('start_time'), chap.get('end_time')
            harvest.toc_add(Gst.TocEntryType.CHAPTER, title, start, end)

    # Find and merge headers for requested formats, then add them
    req_headers = {}
    if (req_formats := info.get('requested_formats') or info.get('requested_downloads')):
        for fmt in req_formats:
            if (hdrs := fmt.get('http_headers')):
                req_headers.update(hdrs)
    if (hdrs := info.get('http_headers')):
        req_headers.update(hdrs)

    if debug.level >= Gst.DebugLevel.DEBUG:
        json_str = json.dumps(req_headers, indent=4)
        debug.print_leveled(Gst.DebugLevel.DEBUG, f'Merged HTTP headers: {json_str}')

    [harvest.headers_set(key, val) for key, val in req_headers.items()]

def playlist_item_add_tags(item: Clapper.MediaItem, entry):
    tags = Gst.TagList.new_empty()
    tags.set_scope(Gst.TagScope.GLOBAL)

    # Add tags useful for a queued item (before playback)
    if (val := entry.get('title')):
        tags.add_value(Gst.TagMergeMode.REPLACE, Gst.TAG_TITLE, val)
    if (val := entry.get('duration')):
        value = GObject.Value()
        value.init(GObject.TYPE_UINT64)
        value.set_uint64(val * Gst.SECOND)
        tags.add_value(Gst.TagMergeMode.REPLACE, Gst.TAG_DURATION, value)
    if (val := entry.get('channel')):
        tags.add_value(Gst.TagMergeMode.REPLACE, Gst.TAG_ARTIST, val)
    if (th := entry.get('thumbnails')) and (best := _find_best_thumbnail(th)):
        # GStreamer supports "text/uri-list" as image sample
        # (see gst_tag_image_data_to_image_sample docs), so
        # lets avoid fetching image for each playlist item here
        data = best['url'].encode('utf-8')
        if (val := GstTag.tag_image_data_to_image_sample(data, GstTag.TagImageType.NONE)):
            tags.add_value(Gst.TagMergeMode.REPLACE, Gst.TAG_PREVIEW_IMAGE, val)

    item.populate_tags(tags)
