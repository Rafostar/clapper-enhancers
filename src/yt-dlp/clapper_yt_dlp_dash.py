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

import xml.etree.ElementTree as ET
from math import gcd

def _get_aspect_ratio(width, height):
    div = gcd(width, height)
    return f'{width // div}:{height // div}'

def _insert_representations(adapt, formats):
    for fmt in formats:
        rep = ET.SubElement(adapt, 'Representation')
        rep.set('id', fmt['format_id'])
        rep.set('bandwidth', str(int(fmt['tbr'] * 1000))) # bps

        vcodec = None if fmt['vcodec'] == 'none' else fmt['vcodec']
        acodec = None if fmt['acodec'] == 'none' else fmt['acodec']

        if vcodec and acodec:
            rep.set('codecs', f'{vcodec},{acodec}')
        elif not acodec:
            rep.set('codecs', vcodec)
        elif not vcodec:
            rep.set('codecs', acodec)

        if vcodec:
            if (width := fmt.get('width') or 0) > 0:
                rep.set('width', str(width))
            if (height := fmt.get('height') or 0) > 0:
                rep.set('height', str(height))
                if width > 0:
                    rep.set('sar', _get_aspect_ratio(width, height))
            if (fps := fmt.get('fps') or 0) > 0:
                rep.set('frameRate', str(fps))

        if acodec and (aud_ch := fmt.get('audio_channels') or 0) > 0:
            ch_conf = ET.SubElement(rep, 'AudioChannelConfiguration')
            ch_conf.set('schemeIdUri', 'urn:mpeg:dash:23003:3:audio_channel_configuration:2011')
            ch_conf.set('value', str(aud_ch))

        base_url = ET.SubElement(rep, 'BaseURL')
        base_url.text = fmt['url']

        # Existence ensured before calling this function
        streaming = fmt['streaming_options']
        init_range = streaming['init_range']
        index_range = streaming['index_range']

        initialization = ET.SubElement(rep, 'Initialization')
        initialization.set('range', init_range)

        segment_base = ET.SubElement(rep, 'SegmentBase')
        segment_base.set('indexRange', index_range)
        segment_base.set('indexRangeExact', 'true')

def _add_adaptation_set(period, info, vcoding, acoding, lang=None):
    max_w = 0
    max_h = 0
    max_fps = 0
    ext = None
    formats = []

    for fmt in info['formats']:
        # Ensure all required values before creating an adaptation set
        if (
                (lang and fmt.get('language') != lang)
                or not ((val := fmt.get('container')) and val.endswith('_dash'))
                or not fmt.get('vcodec', 'none').startswith(vcoding)
                or not fmt.get('acodec', 'none').startswith(acoding)
                or not ((val := fmt.get('ext')) and not val == 'none')
                or not ((val := fmt.get('format_id')) and not val.endswith('-drc'))
                or not (val := fmt.get('tbr') or 0) > 0
        ):
            continue

        if (url := fmt.get('url')): # isoff-on-demand
            if (
                    not (streaming := fmt.get('streaming_options'))
                    or not streaming.get('init_range')
                    or not streaming.get('index_range')
            ):
                continue
        else: # FIXME: isoff-live
            continue

        if vcoding != 'none':
            # Ignore ultralow video qualities
            if (height := fmt.get('height') or 0) < 240:
                continue

            width = fmt.get('width') or 0
            fps = fmt.get('fps') or 0

            max_w = max(max_w, width)
            max_h = max(max_h, height)
            max_fps = max(max_fps, fps)

        if acoding != 'none':
            # Ignore ultralow audio qualities
            if (val := fmt.get('format_note')) == 'ultralow':
                continue

        # All is good
        formats.append(fmt)

    if len(formats) == 0:
        return False

    # yt-dlp uses 'm4a' while DASH expects 'mp4'
    ext = formats[0]['ext']
    ext = {'m4a': 'mp4'}.get(ext, ext)

    cnt_type = 'video' if vcoding != 'none' else 'audio'

    adapt = ET.SubElement(period, 'AdaptationSet')
    adapt.set('contentType', cnt_type)
    adapt.set('mimeType', f'{cnt_type}/{ext}')
    adapt.set('subsegmentAlignment', 'true')
    adapt.set('subsegmentStartsWithSAP', '1')

    if max_w > 0:
        adapt.set('maxWidth', str(max_w))
    if max_h > 0:
        adapt.set('maxHeight', str(max_h))
    if max_w > 0 and max_h > 0:
        div = gcd(max_w, max_h)
        adapt.set('par', f'{max_w // div}:{max_h // div}')
    if max_fps > 0:
        adapt.set('maxFrameRate', str(max_fps))
    if lang:
        # Convert to ISO-639
        adapt.set('lang', lang.split('-')[0])

    _insert_representations(adapt, formats)

    return True

def _add_audio_adaptation_sets(period, info, acoding):
    success = False
    languages = []

    # Find languages and determine default one
    preference = 0
    default_lang = None
    for fmt in info['formats']:
        if not (lang := fmt.get('language')):
            continue

        if (lang_pref := fmt.get('language_preference') or 0) > preference:
            # Append remembered language if not in array yet before replacement
            if default_lang and default_lang not in languages:
                languages.append(default_lang)

            # Replace with new default
            default_lang = lang
            preference = lang_pref
        elif lang != default_lang and lang not in languages:
            languages.append(lang)

    if default_lang:
        languages.insert(0, default_lang)

    # Add an adaptiation set for each language
    for lang in languages:
        success |= _add_adaptation_set(period, info, 'none', acoding, lang)

    # Fallback for undetermined language
    if not success:
        success |= _add_adaptation_set(period, info, 'none', acoding)

    return success

def generate_manifest(info):
    # Check if DASH is requested
    is_dash = False
    separate = False
    if (req_formats := info.get('requested_formats')):
        separate = (len(req_formats) > 1)
        is_dash = ((val := req_formats[0].get('container')) and val.endswith('_dash'))
    else:
        is_dash = ((val := info.get('container')) and val.endswith('_dash'))

    if not is_dash:
        return None

    vcoding = (info.get('vcodec') or 'none')[:4]
    acoding = (info.get('acodec') or 'none')[:4]

    if vcoding == acoding or (separate and 'none' in {vcoding, acoding}):
        return None

    # Duration is required in non-live manifest
    if not (duration := int(info.get('duration') or 0)):
        return None

    ns_xsi = 'http://www.w3.org/2001/XMLSchema-instance'

    ET.register_namespace('xsi', ns_xsi)
    qname = ET.QName(ns_xsi, 'schemaLocation')

    dur_pts = f'PT{duration}S'
    buf_pts = f'PT{min(2, duration)}S'

    mpd = ET.Element('MPD')
    mpd.set(qname, 'urn:mpeg:dash:schema:mpd:2011 DASH-MPD.xsd')
    mpd.set('xmlns', 'urn:mpeg:dash:schema:mpd:2011')
    mpd.set('type', 'static')
    mpd.set('mediaPresentationDuration', dur_pts)
    mpd.set('minBufferTime', buf_pts)
    mpd.set('profiles', 'urn:mpeg:dash:profile:isoff-on-demand:2011')

    period = ET.SubElement(mpd, 'Period')
    success = False

    if separate:
        success |= _add_adaptation_set(period, info, vcoding, 'none')
        success |= _add_audio_adaptation_sets(period, info, acoding)
    else:
        success = _add_adaptation_set(period, info, vcoding, acoding)

    if not success:
        return None

    ET.indent(mpd)

    return ET.tostring(mpd, encoding='unicode', xml_declaration=True)
