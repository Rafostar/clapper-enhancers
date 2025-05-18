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

import os, fnmatch, gi
gi.require_version('GLib', '2.0')
gi.require_version('GObject', '2.0')
gi.require_version('Gio', '2.0')
gi.require_version('Gst', '1.0')
gi.require_version('Clapper', '0.0')
from gi.repository import GLib, GObject, Gio, Gst, Clapper

debug_level = Gst.DebugLevel.NONE
for entry in os.getenv('GST_DEBUG', '').split(','):
    if ':' in entry:
        pattern, level = entry.rsplit(':', 1)
        if fnmatch.fnmatch('clapperytdlp', pattern):
            try: debug_level = int(level)
            except ValueError: continue

if debug_level >= Gst.DebugLevel.LOG:
    import json

try:
    from yt_dlp import YoutubeDL
except ImportError:
    YoutubeDL = None

if YoutubeDL:
    from yt_dlp.extractor import gen_extractor_classes
    from clapper_yt_dlp_overrides import BLACKLIST, ClapperYoutubeIE

import clapper_yt_dlp_dash as dash
import clapper_yt_dlp_hls as hls
import clapper_yt_dlp_direct as direct

YTDL_OPTS = {
    'verbose': debug_level >= Gst.DebugLevel.DEBUG,
    'quiet': debug_level < Gst.DebugLevel.INFO,
    'color': 'never', # no color in exceptions
    'ignoreconfig': True,
    'format': 'bestvideo[protocol*=m3u8]+bestaudio[protocol*=m3u8]/bestvideo[container*=dash]+bestaudio[container*=dash]/best',
    'extract_flat': 'in_playlist',
    'extractor_args': {
        'youtube': {
            'skip': ['translated_subs'],
            'player_client': ['ios']
        }
    }
}

EXPIRATIONS = {
    'youtube': 900, # 15 minutes
    'default': 180 # 3 minutes
}

class ClapperYtDlp(GObject.Object, Clapper.Extractable):
    if Clapper.MINOR_VERSION >= 9:
        codecs_order = GObject.Property(type=str, nick='Codecs Order',
            blurb='Comma-separated order of preferred video codecs',
            default='avc1,av01,hev1,vp09',
            flags=(GObject.ParamFlags.READWRITE | Clapper.EnhancerParamFlags.GLOBAL)
        )
    else:
        codecs_order = 'avc1,av01,hev1,vp09'

    _ytdl = None

    def __init__(self):
        if YoutubeDL:
            self._ytdl = YoutubeDL(YTDL_OPTS, auto_init=False)

            self._ytdl.add_info_extractor(ClapperYoutubeIE())

            for ie in gen_extractor_classes():
                if ie.ie_key() not in BLACKLIST:
                    self._ytdl.add_info_extractor(ie)

    def do_extract(self, uri: GLib.Uri, harvest: Clapper.Harvest, cancellable: Gio.Cancellable):
        if not YoutubeDL:
            raise GLib.Error('Could not import "yt-dlp". Please check your installation.')

        # Not used during init, so we can alter it here
        self._ytdl.params['noplaylist'] = True
        self._ytdl.params['format_sort'] = ['vcodec:' + c.strip() for c in self.codecs_order.split(',')]

        try:
            info = self._ytdl.extract_info(uri.to_string(), download=False)
        except Exception as e:
            raise GLib.Error(str(e))

        # Check if cancelled during extraction
        if cancellable.is_cancelled():
            return False

        if debug_level >= Gst.DebugLevel.LOG:
            print(json.dumps(self._ytdl.sanitize_info(info), indent=4))

        if (manifest := hls.generate_manifest(info)):
            media_type = 'application/x-hls'
        elif (manifest := dash.generate_manifest(info)):
            media_type = 'application/dash+xml'
        elif (manifest := direct.generate_manifest(info)):
            media_type = 'text/uri-list'
        else:
            raise GLib.Error('Could not generate playable manifest')

        # Check if cancelled during manifest generation
        if cancellable.is_cancelled():
            return False

        harvest.fill_with_text(media_type, manifest)

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
        for fmt in info['formats']:
            if (hdrs := fmt.get('http_headers')):
                [harvest.headers_set(key, val) for key, val in hdrs.items()]
                break

        if Clapper.MINOR_VERSION >= 9 and not info.get('is_live'):
            harvest.set_expiration_seconds(EXPIRATIONS.get(info.get('extractor'), EXPIRATIONS['default']))

        return True
