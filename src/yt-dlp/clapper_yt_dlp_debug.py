# Clapper Enhancer yt-dlp
# Copyright (C) 2025 Rafał Dzięgiel <rafostar.github@gmail.com>
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

import os, fnmatch, gi
gi.require_version('Gst', '1.0')
gi.require_version('Gio', '2.0')
from gi.repository import Gio, Gst

level = Gst.DebugLevel.NONE
for entry in os.getenv('GST_DEBUG', '').split(','):
    if ':' in entry:
        pattern, lev = entry.rsplit(':', 1)
        if fnmatch.fnmatch('clapperytdlp', pattern):
            try: level = int(lev)
            except ValueError: continue

def print_leveled(lev, text):
    if level >= lev:
        print(f'[clapper_yt_dlp] {text}')

class ClapperYtDlpLogger:
    """
    Custom logger also checks cancellable state while yt-dlp
    operates and rises error when cancelled cause we do not
    have any other means to tell it to cancel info extraction
    """
    def __init__(self, cancellable: Gio.Cancellable):
        self._cancellable = cancellable

    def _check_cancelled(self):
        if self._cancellable.is_cancelled():
            raise Exception('Extraction cancelled')

    def info(self, text):
        self._check_cancelled()
        print_leveled(Gst.DebugLevel.INFO, text)

    def debug(self, text):
        self._check_cancelled()
        # yt-dlp calls this for all kinds of messages (compat reasons)
        if text.startswith('[debug]'):
            print_leveled(Gst.DebugLevel.DEBUG, text)
        elif text.startswith('[info]'):
            print_leveled(Gst.DebugLevel.INFO, text)
        else:
            print_leveled(Gst.DebugLevel.LOG, text)

    def warning(self, text):
        self._check_cancelled()
        print_leveled(Gst.DebugLevel.WARNING, text)

    def error(self, text):
        self._check_cancelled()
        print_leveled(Gst.DebugLevel.ERROR, text)
