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

from yt_dlp.extractor.youtube import YoutubeIE
from yt_dlp.utils import parse_m3u8_attributes

BLACKLIST = [
    'Youtube',
    'Generic'
]

class ClapperYoutubeIE(YoutubeIE):
    @classmethod
    def ie_key(cls):
        return cls.__name__[7:-2]

    def _extract_formats_and_subtitles(self, streaming_data, video_id, player_url, live_status, duration):
        *formats, subtitles = super()._extract_formats_and_subtitles(streaming_data, video_id, player_url, live_status, duration)

        for fmt in formats:
            if not (format_id := fmt.get('format_id')):
                continue

            expires_in = 0

            for resp_data in streaming_data:
                resp_found = False

                for yt_fmt in resp_data.get('adaptiveFormats'):
                    if (
                            # FIXME: Watch out for the same itag with different language
                            not format_id.startswith(str(yt_fmt.get('itag')))
                            or yt_fmt.get('isDrc', False)
                    ):
                        continue

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
                    expires_in = int(resp_data.get('expiresInSeconds') or 0)

                    resp_found = True
                    break

                if resp_found:
                    break

            fmt['expires_in'] = expires_in

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
                if not (format_id := fmt.get('format_id')):
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
