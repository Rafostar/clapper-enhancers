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
gi.require_version('Clapper', '0.0')
from gi.repository import GLib, GObject, Gio, Gst, Clapper

import clapper_yt_dlp_debug as debug

def _fetch_image_sample(thumbnails, cancellable: Gio.Cancellable):
    best = None
    best_ext = None
    best_area = 1

    debug.print_leveled(Gst.DebugLevel.DEBUG, 'Fetching image sample...')

    for t in thumbnails:
        width = t.get('width') or 0
        height = t.get('height') or 0

        if (area := width * height) < best_area or not (url := t.get('url')):
            continue

        ext = os.path.splitext(url.split('?')[0])[1][1:].lower()
        ext = {'jpg': 'jpeg'}.get(ext, ext)

        # Extensions for "image/" caps
        if ext in ['jpeg', 'webp', 'png']:
            best = t
            best_ext = ext
            best_area = area

    if not best:
        return None

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

    debug.print_leveled(Gst.DebugLevel.DEBUG, 'Fetched image sample')

    buffer = Gst.Buffer.new_wrapped_bytes(gbytes)

    caps = Gst.Caps.new_empty_simple(f'image/{best_ext}')
    caps.set_value('width', best['width'])
    caps.set_value('height', best['height'])

    return Gst.Sample.new(buffer, caps, None, None)

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

    item.populate_tags(tags)
