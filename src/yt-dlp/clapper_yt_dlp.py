# -*- coding: utf-8 -*-

# Clapper Enhancer yt-dlp
# Copyright (C) 2024 Rafał Dzięgiel <rafostar.github@gmail.com>
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
# License along with this library; if not, write to the
# Free Software Foundation, Inc., 51 Franklin St, Fifth Floor,
# Boston, MA 02110-1301, USA.

import gi
gi.require_version('GLib', '2.0')
gi.require_version('GObject', '2.0')
gi.require_version('Gio', '2.0')
gi.require_version('Gst', '1.0')
gi.require_version('Clapper', '0.0')
from gi.repository import GLib, GObject, Gio, Gst, Clapper

from yt_dlp import YoutubeDL
from yt_dlp.extractor import gen_extractor_classes

from clapper_yt_dlp_overrides import BLACKLIST, ClapperYoutubeIE
import clapper_yt_dlp_dash as dash
import clapper_yt_dlp_hls as hls

YTDL_OPTS = {
    'quiet': True,
    'color': 'never', # no color in exceptions
    'ignoreconfig': True,
    'extract_flat': 'in_playlist',
    'extractor_args': {
        'youtube': {
            'skip': ['translated_subs'],
            'player_client': ['ios'],
            'player_skip': ['webpage', 'configs', 'js']
        }
    }
}

class ClapperYtDlp(GObject.Object, Clapper.Extractable):
    _ytdl = None

    def __init__(self):
        self._ytdl = YoutubeDL(YTDL_OPTS, auto_init=False)

        self._ytdl.add_info_extractor(ClapperYoutubeIE())

        for ie in gen_extractor_classes():
            if ie.ie_key() not in BLACKLIST:
                self._ytdl.add_info_extractor(ie)

    def do_extract(self, uri: GLib.Uri, harvest: Clapper.Harvest, cancellable: Gio.Cancellable):
        # Not used during init, so we can alter it here
        self._ytdl.params['noplaylist'] = True

        try:
            info = self._ytdl.extract_info(uri.to_string(), download=False)
        except Exception as e:
            raise GLib.Error(str(e))

        # Check if cancelled during extraction
        if cancellable.is_cancelled():
            return False

        if (manifest := dash.generate_manifest(info)):
            media_type = 'application/dash+xml'
        elif (manifest := hls.generate_manifest(info)):
            media_type = 'application/x-hls'
        else:
            raise GLib.Error('Could not generate playable manifest')

        # Check if cancelled during manifest generation
        if cancellable.is_cancelled():
            return False

        harvest.fill(media_type, manifest)

        if (val := info.get('title')):
            harvest.tags_add(Gst.TAG_TITLE, val)
        if (val := info.get('duration')):
            value = GObject.Value()
            value.init(GObject.TYPE_UINT64)
            value.set_uint64(val * Gst.SECOND)
            harvest.tags_add(Gst.TAG_DURATION, value)
        if (val := info.get('chapters')):
            for index, chap in enumerate(val):
                title, start, end = chap.get('title'), chap.get('start_time'), chap.get('end_time')
                harvest.toc_add(Gst.TocEntryType.CHAPTER, title, start, end)

        # XXX: We just take headers from any format here, do we need to find/combine some?
        if (req_fmt := info.get('requested_formats')) and (hdrs := req_fmt[0].get('http_headers')):
            [harvest.headers_set(key, val) for key, val in hdrs.items()]

        return True
