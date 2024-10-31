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

from yt_dlp.extractor.youtube import YoutubeIE

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

            for resp_data in streaming_data:
                expires_in = int(resp_data.get('expiresInSeconds') or 0)
                adaptive_formats = resp_data.get('adaptiveFormats')

                fmt['downloader_options']['expires_in'] = expires_in

                for yt_fmt in adaptive_formats:
                    if (
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

                    fmt['downloader_options'].update({
                        'init_range': f'{init_start}-{init_end}',
                        'index_range': f'{index_start}-{index_end}'
                    })

        return *formats, subtitles
