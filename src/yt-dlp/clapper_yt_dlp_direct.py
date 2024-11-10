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

def _make_manifest(info, vext):
    best_format = None

    for fmt in info['formats']:
        if (
                fmt.get('protocol') != 'https'
                or fmt.get('video_ext') != vext
                or not fmt.get('url')
        ):
            continue

        if not best_format:
            best_format = fmt
            continue

        if vext != 'none':
            height = fmt.get('height') or 0
            best_height = best_format.get('height') or 0

            fps = fmt.get('fps') or 0
            best_fps = best_format.get('fps') or 0

            if (
                    height > best_height
                    or (height == best_height and fps > best_fps)
            ):
                best_format = fmt
                continue

        tbr = fmt.get('tbr') or 0
        best_tbr = best_format.get('tbr') or 0

        if (tbr > best_tbr):
            best_format = fmt

    if not best_format:
        return None

    manifest = io.StringIO()
    manifest.write(best_format['url'])

    return manifest

def generate_manifest(info):
    if (
            (manifest := _make_manifest(info, 'mp4'))
            or (manifest := _make_manifest(info, 'none')) # Audio only
    ):
        return manifest.getvalue()

    return None
