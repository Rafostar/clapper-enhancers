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
# License along with this library; if not, see
# <https://www.gnu.org/licenses/>.

import os, fnmatch, json, gi
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
import clapper_yt_dlp_playlist as playlist

# NOTE: GStreamer does not support mp3 and opus in HLS yet
FORMAT_PREFERENCE = '/'.join([
    'bestvideo[protocol*=m3u8]+bestaudio[protocol*=m3u8][ext!=mp3][ext!=opus]',
    'bestvideo[container*=dash]+bestaudio[container*=dash]',
    'bestaudio[protocol*=m3u8][ext!=mp3][ext!=opus]',
    'bestaudio[container*=dash]',
    'best[protocol*=http]',
    'best'
])

YTDL_OPTS = {
    'verbose': debug_level >= Gst.DebugLevel.DEBUG,
    'quiet': debug_level < Gst.DebugLevel.INFO,
    'color': 'never', # no color in exceptions
    'ignoreconfig': True,
    'format': FORMAT_PREFERENCE,
    'extract_flat': 'in_playlist',
    'noplaylist': True,
    'extractor_args': {
        'youtube': {
            'skip': ['translated_subs'],
            'player_client': ['ios']
        },
        'youtubetab': {
            'skip': ['webpage']
        }
    }
}

EXPIRATIONS = {
    'youtube': 900, # 15 minutes
    'youtube:search_url': 300, # 5 minutes
    'youtube:tab': 600, # 10 minutes (playlist/channel)
    'default': 180 # 3 minutes
}

class ClapperYtDlp(GObject.Object, Clapper.Extractable, Clapper.Playlistable):
    if Clapper.MINOR_VERSION >= 9:
        codecs_order = GObject.Property(type=str, nick='Codecs Order',
            blurb='Comma-separated order of preferred video codecs',
            default='avc1,av01,hev1,vp09',
            flags=(GObject.ParamFlags.READWRITE | Clapper.EnhancerParamFlags.GLOBAL)
        )
        cookies_file = GObject.Property(type=str, nick='Cookies File',
            blurb='Netscape formatted file to read and write cookies',
            default='',
            flags=(GObject.ParamFlags.READWRITE | Clapper.EnhancerParamFlags.GLOBAL | Clapper.EnhancerParamFlags.FILEPATH)
        )
    else:
        codecs_order = 'avc1,av01,hev1,vp09'
        cookies_file = ''

    _ytdl = None

    def __init__(self):
        if YoutubeDL:
            self._ytdl = YoutubeDL(YTDL_OPTS, auto_init=False)

            self._ytdl.add_info_extractor(ClapperYoutubeIE())

            for ie in gen_extractor_classes():
                if ie._ENABLED and ie.ie_key() not in BLACKLIST:
                    self._ytdl.add_info_extractor(ie)

    def do_extract(self, uri: GLib.Uri, harvest: Clapper.Harvest, cancellable: Gio.Cancellable):
        if not YoutubeDL:
            raise GLib.Error('Could not import "yt-dlp". Please check your installation.')

        # Not used during init, so we can alter it here
        self._ytdl.params['format_sort'] = ['vcodec:' + c.strip() for c in self.codecs_order.split(',')]
        if self.cookies_file:
            if os.path.isfile(self.cookies_file):
                self._ytdl.params['cookies'] = self.cookies_file
            else:
                raise GLib.Error('Specified cookies file does not exist')

        # FIXME: Can this be improved somehow (considering other websites)?
        # Limit extraction to first 50 items if not a playlist
        if not uri.get_path().startswith('/playlist'):
            self._ytdl.params['playlist_items'] = '0:50'
            if debug_level >= Gst.DebugLevel.DEBUG:
                print('[clapper_yt_dlp] Extraction range limited to first 50 items')

        uri_str = uri.to_string()

        # Replace custom "ytdlp" scheme with "https"
        if uri_str.startswith("ytdlp://"):
            uri_str = "https" + uri_str[5:]

        try:
            info = self._ytdl.extract_info(uri_str, download=False)
        except Exception as e:
            raise GLib.Error(str(e))

        # Check if cancelled during extraction
        if cancellable.is_cancelled():
            return False

        if debug_level >= Gst.DebugLevel.LOG:
            print(json.dumps(self._ytdl.sanitize_info(info), indent=4))

        is_playlist = False

        if (manifest := hls.generate_manifest(info)):
            media_type = 'application/x-hls'
        elif (manifest := dash.generate_manifest(info)):
            media_type = 'application/dash+xml'
        elif (manifest := direct.generate_manifest(info)):
            media_type = 'text/x-uri'
        elif (manifest := playlist.generate_manifest(info)):
            media_type = 'application/clapper-playlist'
            is_playlist = True
        else:
            raise GLib.Error('Could not generate playable manifest')

        # Check if cancelled during manifest generation
        if cancellable.is_cancelled():
            return False

        extractor_name = info.get('extractor')
        if debug_level >= Gst.DebugLevel.DEBUG:
            print(f'[clapper_yt_dlp] Used extractor: "{extractor_name}"')

        harvest.fill_with_text(media_type, manifest)

        if not is_playlist:
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
            harvest.set_expiration_seconds(EXPIRATIONS.get(extractor_name, EXPIRATIONS['default']))

        return True

    def do_parse(self, uri: GLib.Uri, gbytes: GLib.Bytes, plist: Gio.ListStore, cancellable: Gio.Cancellable):
        info = json.loads(gbytes.get_data())

        for entry in info['entries']:
            if cancellable.is_cancelled():
                return False

            # Type is usually "url" or "url_transparent"
            if (
                    not ((val := entry.get('_type')) and val.startswith('url'))
                    or not entry.get('url')
            ):
                continue

            item = Clapper.MediaItem(uri=entry['url'])
            tags = Gst.TagList.new_empty()
            tags.set_scope(Gst.TagScope.GLOBAL)

            if (val := entry.get('title')):
                tags.add_value(Gst.TagMergeMode.REPLACE, Gst.TAG_TITLE, val)
            if (val := entry.get('duration')):
                value = GObject.Value()
                value.init(GObject.TYPE_UINT64)
                value.set_uint64(val * Gst.SECOND)
                tags.add_value(Gst.TagMergeMode.REPLACE, Gst.TAG_DURATION, value)

            item.populate_tags(tags)
            plist.append(item)

        return True
