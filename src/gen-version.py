#
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

import datetime, subprocess, sys

template = open(sys.argv[1]).read()

if sys.argv[3]:
    version = sys.argv[3]
else:
    version = sys.argv[2]
    try:
        tagged = subprocess.check_output(["git", "tag", "--points-at", "HEAD"]).decode().split()
        dirty = subprocess.check_output(["git", "describe", "--dirty"]).decode().strip().endswith("-dirty")
        if not tagged or dirty:
            if dirty:
                timestamp = int(datetime.datetime.utcnow().timestamp())
            else:
                timestamp = subprocess.check_output(["git", "log", "-1", "--format=%ct"]).decode().strip()
            sha = subprocess.check_output(["git", "rev-parse", "--short", "HEAD"]).decode().strip()
            version += f"+git.{timestamp}~{sha}"
            if dirty:
                version += "-dirty"
    except Exception:
        pass

out = template.replace("@VERSION@", version)
sys.stdout.write(out)
