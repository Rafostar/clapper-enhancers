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

import os, json, re, shlex, gi
gi.require_version('GLib', '2.0')
gi.require_version('GObject', '2.0')
gi.require_version('Gio', '2.0')
gi.require_version('Gst', '1.0')
gi.require_version('Clapper', '0.0')
from gi.repository import GLib, GObject, Gio, Gst, Clapper

try:
    from yt_dlp import YoutubeDL
except ImportError:
    YoutubeDL = None

if YoutubeDL:
    from yt_dlp.extractor import gen_extractor_classes
    from clapper_yt_dlp_overrides import BLACKLIST, ClapperYoutubeIE

import clapper_yt_dlp_debug as debug
import clapper_yt_dlp_dash as dash
import clapper_yt_dlp_hls as hls
import clapper_yt_dlp_direct as direct
import clapper_yt_dlp_playlist as playlist
import clapper_yt_dlp_utils as utils

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
    'verbose': debug.level >= Gst.DebugLevel.DEBUG,
    'quiet': debug.level < Gst.DebugLevel.INFO,
    'color': 'never', # no color in exceptions
    'ignoreconfig': True,
    'format': FORMAT_PREFERENCE,
    'extract_flat': 'in_playlist',
    'noplaylist': True,
    'extractor_args': {
        'youtube': {
            'skip': ['translated_subs']
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

# Clapper 0.8 compat
bases = (GObject.Object, Clapper.Extractable)
if Clapper.MINOR_VERSION >= 9:
    bases += (Clapper.Playlistable,)

class ClapperYtDlp(*bases):
    if Clapper.MINOR_VERSION >= 9:
        codecs_order = GObject.Property(type=str, nick='Codecs Order',
            blurb='Comma-separated order of preferred video codecs',
            default='avc1,av01,hev1,vp09',
            flags=(GObject.ParamFlags.READWRITE | Clapper.EnhancerParamFlags.GLOBAL)
        )
        cookies_file = GObject.Property(type=str, nick='Cookies File',
            blurb='Netscape formatted file to read and write cookies',
            default=None,
            flags=(GObject.ParamFlags.READWRITE | Clapper.EnhancerParamFlags.GLOBAL | Clapper.EnhancerParamFlags.FILEPATH)
        )
        extractor_args = GObject.Property(type=str, nick='Extractor Args',
            blurb='Extractor arguments in KEY:ARGS format with whitespace separation for each KEY',
            default=None,
            flags=(GObject.ParamFlags.READWRITE | Clapper.EnhancerParamFlags.GLOBAL | Clapper.EnhancerParamFlags.LOCAL)
        )
    else:
        codecs_order = 'avc1,av01,hev1,vp09'
        cookies_file = None
        extractor_args = None

    def make_extractor_args_dict(self):
        # Based on yt-dlp _extractor_arg_parser
        def _parse_pair(key, vals=""):
            norm_key = key.strip().lower().replace("-", "_")
            items = re.split(r"(?<!\\),", vals)
            return norm_key, [v.replace(r"\,", ",").strip() for v in items]

        result = {}
        # Split on whitespace to get each IE_KEY:ARGS chunk
        for chunk in shlex.split(self.extractor_args):
            # Option names are not required, skip them if present
            if chunk == '--extractor-args':
                continue

            ie_key, ie_args = chunk.split(":", 1)
            # Split on semicolons to get each K=V slice
            subdict = dict(
                _parse_pair(*arg.split("=", 1)) for arg in ie_args.split(";")
            )
            result[ie_key] = subdict

        return result

    def do_extract(self, uri: GLib.Uri, harvest: Clapper.Harvest, cancellable: Gio.Cancellable):
        if not YoutubeDL:
            raise GLib.Error('Could not import "yt-dlp". Please check your installation.')

        # Writable options copy to apply user set properties
        opts = YTDL_OPTS.copy()

        opts['logger'] = debug.ClapperYtDlpLogger(cancellable)
        opts['format_sort'] = (
            ['vcodec:' + c.strip() for c in self.codecs_order.split(',')] +
            ['acodec:mp4a', 'acodec:opus', 'acodec:vorbis', 'acodec:*']
        )
        if self.cookies_file:
            if os.path.isfile(self.cookies_file):
                debug.print_leveled(Gst.DebugLevel.INFO, f'Set cookies: {self.cookies_file}')
                opts['cookies'] = self.cookies_file
            else:
                raise GLib.Error('Specified cookies file does not exist')
        if self.extractor_args:
            try:
                args_dict = self.make_extractor_args_dict()
                debug.print_leveled(Gst.DebugLevel.INFO, f'Set extractor_args: {args_dict}')
                opts['extractor_args'].update(args_dict)
            except Exception as e:
                raise GLib.Error(f'Could not parse extractor-args, reason: {str(e)}')

        # FIXME: Can this be improved somehow (considering other websites)?
        # Limit extraction to first 20 items if not a playlist
        if not uri.get_path().startswith('/playlist'):
            opts['playlist_items'] = '0:20'
            debug.print_leveled(Gst.DebugLevel.DEBUG, 'Extraction range limited to first 20 items')

        uri_str = uri.to_string()

        # Replace custom "ytdlp" scheme with "https"
        if uri_str.startswith("ytdlp://"):
            uri_str = "https" + uri_str[5:]

        ytdl = YoutubeDL(opts, auto_init=False)
        ytdl.add_info_extractor(ClapperYoutubeIE())
        for ie in gen_extractor_classes():
            if ie._ENABLED and ie.ie_key() not in BLACKLIST:
                ytdl.add_info_extractor(ie)

        try:
            info = ytdl.extract_info(uri_str, download=False)
        except Exception as e:
            raise GLib.Error(str(e))

        # Check if cancelled during extraction
        if cancellable.is_cancelled():
            return False

        if debug.level >= Gst.DebugLevel.LOG:
            json_str = json.dumps(ytdl.sanitize_info(info), indent=4)
            debug.print_leveled(Gst.DebugLevel.LOG, 'Extracted info:\n' + json_str)

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
        debug.print_leveled(Gst.DebugLevel.DEBUG, f'Used extractor: "{extractor_name}"')

        harvest.fill_with_text(media_type, manifest)

        if not is_playlist:
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
            if (val := info.get('chapters')):
                for index, chap in enumerate(val):
                    title, start, end = chap.get('title'), chap.get('start_time'), chap.get('end_time')
                    harvest.toc_add(Gst.TocEntryType.CHAPTER, title, start, end)
            if (th := info.get('thumbnails')) and (val := utils.fetch_image_sample(th, cancellable)):
                harvest.tags_add(Gst.TAG_PREVIEW_IMAGE, val)

            # Find and merge headers for requested formats
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
