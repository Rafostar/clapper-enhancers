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

import os, gi
gi.require_version('GLib', '2.0')
gi.require_version('Gio', '2.0')
gi.require_version('Gst', '1.0')
from gi.repository import GLib, Gio, Gst

import clapper_yt_dlp_debug as debug

def fetch_image_sample(thumbnails, cancellable: Gio.Cancellable):
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
