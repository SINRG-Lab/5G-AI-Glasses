"""
Pre-build script: downloads minimp3.h into lib/minimp3/ if not already present.
minimp3 is header-only (https://github.com/lieff/minimp3) — we fetch only the
single header rather than pulling the whole repo (which contains SDL2/OpenGL demo
code that does not compile on embedded targets).
"""

Import("env")  # noqa: F821  (PlatformIO SConscript global)

import os
import urllib.request

HEADER_URL  = "https://raw.githubusercontent.com/lieff/minimp3/master/minimp3.h"
HEADER_PATH = os.path.join(env["PROJECT_DIR"], "lib", "minimp3", "minimp3.h")  # noqa

if not os.path.exists(HEADER_PATH):
    print("minimp3.h not found — downloading from GitHub...")
    try:
        urllib.request.urlretrieve(HEADER_URL, HEADER_PATH)
        print(f"  Saved to {HEADER_PATH}")
    except Exception as e:
        print(f"  ERROR: could not download minimp3.h: {e}")
        print(f"  Please manually download {HEADER_URL}")
        print(f"  and save it to {HEADER_PATH}")
