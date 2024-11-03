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

import io

def _add_x_media(manifest, fmt):
    acodec = None if fmt['acodec'] == 'none' else fmt['acodec']
    media_type = 'AUDIO' if acodec else 'CLOSED-CAPTIONS'

    manifest.write(f'#EXT-X-MEDIA:TYPE={media_type}'.encode())

    fmt_id = fmt['format_id']
    manifest.write(f',GROUP-ID="{fmt_id}"'.encode())

    if (language := fmt.get('language')):
        manifest.write(f',LANGUAGE="{language}"'.encode())

    fmt_name = fmt.get('format_note', 'Default')
    manifest.write(f',NAME="{fmt_name}",DEFAULT=YES,AUTOSELECT=YES'.encode())

    url = fmt['url']
    manifest.write(f',URI="{url}"\n'.encode())

def _add_x_stream_inf(manifest, fmt, audio, captions):
    # Existence ensured before calling this function
    bandwidth = int(fmt['tbr'] * 1000)
    manifest.write(f'#EXT-X-STREAM-INF:BANDWIDTH={bandwidth}'.encode())

    vcodec = None if fmt['vcodec'] == 'none' else fmt['vcodec']
    acodec = None if fmt['acodec'] == 'none' else fmt['acodec']

    if not acodec and audio:
        acodec = None if audio['acodec'] == 'none' else audio['acodec']

    if vcodec and acodec:
        manifest.write(f',CODECS="{vcodec},{acodec}"'.encode())
    else:
        codec = vcodec if vcodec else acodec
        manifest.write(f',CODECS="{codec}"'.encode())

    width = fmt.get('width') or 0
    height = fmt.get('height') or 0
    if width > 0 and height > 0:
        manifest.write(f',RESOLUTION={width}x{height}'.encode())

    if ((fps := fmt.get('fps') or 0) > 0):
        manifest.write(f',FRAME-RATE={fps}'.encode())

    if (dyn_range := fmt.get('dynamic_range')):
        manifest.write(f',VIDEO-RANGE={dyn_range}'.encode())

    if vcodec and audio:
        audio_id = audio['format_id']
        manifest.write(f',AUDIO="{audio_id}"'.encode())

    if vcodec and captions:
        captions_id = captions['format_id']
        manifest.write(f',CLOSED-CAPTIONS="{captions_id}"'.encode())

    url = fmt['url']
    manifest.write(f'\n{url}\n'.encode())

def _insert_streams(manifest, formats, matches):
    for fmt in formats:
        have_bandwidth = (fmt.get('tbr') != None)

        if have_bandwidth:
            audio = None
            captions = None

            if (audio_id := fmt.get('audio_id')):
                audio = next(filter(lambda m: m['format_id'] == audio_id, matches), None)
            if (captions_id := fmt.get('captions_id')):
                captions = next(filter(lambda m: m['format_id'] == captions_id, matches), None)

            _add_x_stream_inf(manifest, fmt, audio, captions)
        else:
            _add_x_media(manifest, fmt)

def _add_streams(manifest, info, vcoding, acoding):
    formats = []
    matches = []

    for fmt in info['formats']:
        # Ensure all required values before creating adding stream
        if (
                not ((val := fmt.get('protocol')) and val == 'm3u8_native')
                or not fmt.get('url')
                or not fmt.get('vcodec', 'none').startswith(vcoding)
                or not fmt.get('acodec', 'none').startswith(acoding)
                or not ((val := fmt.get('format_id')) and not val.endswith('-drc'))
        ):
            continue

        if vcoding != 'none':
            # Ignore ultralow video qualities
            if (height := fmt.get('height') or 0) < 240:
                continue

            if ((audio_id := fmt.get('audio_id')) and not any(m['format_id'] == audio_id for m in matches)):
                for audio_fmt in info['formats']:
                    if (audio_fmt.get('format_id') == audio_id):
                        matches.append(audio_fmt)

        if acoding != 'none':
            # Ignore ultralow audio qualities
            if (val := fmt.get('format_note')) == 'ultralow':
                continue

        # All is good
        formats.append(fmt)

    if len(formats) == 0:
        return False

    _insert_streams(manifest, formats, matches)

    return True

def generate_manifest(info):
    manifest = io.BytesIO()
    success = False

    manifest.write('#EXTM3U\n#EXT-X-INDEPENDENT-SEGMENTS\n'.encode())

    # Start with audio stream as video streams often point to audio
    success |= _add_streams(manifest, info, 'none', 'mp4a')
    success |= _add_streams(manifest, info, 'avc1', 'none')

    if not success:
        success = _add_streams(manifest, info, 'avc1', 'mp4a')

    if not success:
        return None

    manifest.seek(0)

    return manifest.read()
