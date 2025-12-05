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

import re, gi
gi.require_version('Gst', '1.0')
from gi.repository import Gst

from yt_dlp.extractor.youtube import YoutubeIE
from yt_dlp.utils import parse_m3u8_attributes, traverse_obj

import clapper_yt_dlp_debug as debug

BLACKLIST = [
    'Youtube'
]

class ClapperYoutubeIE(YoutubeIE):
    @classmethod
    def ie_key(cls):
        return cls.__name__[7:-2]

    _ITAG_PATTERN = re.compile(r'/itag/(\d+)')

    def _extract_formats_and_subtitles(self, video_id, player_responses, player_url, live_status, duration):
        *formats, subtitles = super()._extract_formats_and_subtitles(video_id, player_responses, player_url, live_status, duration)

        for fmt in formats:
            if not (format_id := fmt.get('format_id')):
                continue

            itag = format_id.split('-')[0]
            lang = (language := fmt.get('language')) and language.split('-')[0]

            debug.print_leveled(Gst.DebugLevel.DEBUG, f'Processing format ID: {format_id} (itag: {itag})')

            for pr in player_responses:
                if not (
                        (resp_data := traverse_obj(pr, 'streamingData'))
                        and (adapt_formats := resp_data.get('adaptiveFormats'))
                ):
                    continue

                resp_found = False

                for yt_fmt in adapt_formats:
                    # Find format with the same itag
                    if (
                            itag != str(yt_fmt.get('itag') or 0)
                            or yt_fmt.get('isDrc', False)
                    ):
                        continue

                    # Special cases in YT formats: "en.4"
                    yt_lang = (yt_atrack := yt_fmt.get('audioTrack')) and yt_atrack['id'].split('-')[0].split('.')[0]

                    # Youtube uses the same itag for different languages, so language must also match
                    if (lang != yt_lang):
                        continue

                    debug.print_leveled(Gst.DebugLevel.DEBUG, 'Found matching YT format')

                    init_range = yt_fmt.get('initRange')
                    index_range = yt_fmt.get('indexRange')

                    if not init_range or not index_range:
                        continue

                    init_start = int(init_range.get('start') or 0)
                    init_end = int(init_range.get('end') or 0)

                    index_start = int(index_range.get('start') or 0)
                    index_end = int(index_range.get('end') or 0)

                    if init_end <= init_start or index_end <= index_start:
                        continue

                    fmt['streaming_options'] = {
                        'init_range': f'{init_start}-{init_end}',
                        'index_range': f'{index_start}-{index_end}'
                    }
                    debug.print_leveled(Gst.DebugLevel.DEBUG, 'Added format streaming options')

                    resp_found = True
                    break

                if resp_found:
                    break

        return *formats, subtitles

    def _parse_m3u8_formats_and_subtitles(
            self, m3u8_doc, m3u8_url=None, ext=None, entry_protocol='m3u8_native',
            preference=None, quality=None, m3u8_id=None, live=False, note=None,
            errnote=None, fatal=True, data=None, headers={}, query={},
            video_id=None):
        formats, subtitles = super()._parse_m3u8_formats_and_subtitles(
                m3u8_doc, m3u8_url, ext, entry_protocol,
                preference, quality, m3u8_id, live, note,
                errnote, fatal, data, headers, query,
                video_id)

        for line in m3u8_doc.splitlines():
            if not line.startswith('#EXT-X-STREAM-INF:'):
                continue

            attributes = parse_m3u8_attributes(line)
            bandwidth = attributes.get('BANDWIDTH')
            audio_id = attributes.get('AUDIO')
            captions_id = attributes.get('CLOSED-CAPTIONS')

            if not bandwidth or not (audio_id or captions_id):
                continue

            for fmt in formats:
                # Format ID might not be extracted here yet
                if not (format_id := fmt.get('format_id')):
                    match = self._ITAG_PATTERN.search(fmt['url'])
                    if not (format_id := match and match.group()):
                        continue

                group_id = format_id.split('-')[0]

                # Fix missing audio codecs
                if (group_id == audio_id and fmt.get('acodec', 'none') == 'none'):
                    codecs_arr = attributes.get('CODECS').split(',')

                    if len(codecs_arr) > 1:
                        fmt['acodec'] = codecs_arr[1]

                # Fix unknown audio/captions stream for video
                if int((fmt.get('tbr') or 0) * 1000) == int(bandwidth):
                    fmt['audio_id'] = audio_id
                    fmt['captions_id'] = captions_id

        return formats, subtitles
