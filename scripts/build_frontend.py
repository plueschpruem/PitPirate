"""
PlatformIO extra script — runs `npm run build` inside frontend/ before
building the LittleFS image, so `pio run -t uploadfs` always picks up
the latest Vue build.
"""
Import("env")  # type: ignore  # noqa: F821  (PlatformIO SCons environment)

import os
import subprocess
import sys

_built = False  # guard: only run once per PlatformIO invocation


def build_frontend(*_args, **_kwargs):
    global _built
    if _built:
        return
    _built = True

    frontend_dir = os.path.join(env["PROJECT_DIR"], "frontend")  # type: ignore[name-defined]
    if not os.path.isdir(frontend_dir):
        print(">>> scripts/build_frontend.py: no frontend/ directory found, skipping.")
        return

    print(">>> Building Vue frontend (npm run build)…")
    result = subprocess.run(["npm", "run", "build"], cwd=frontend_dir)
    if result.returncode != 0:
        print("ERROR: frontend build failed!", file=sys.stderr)
        env.Exit(1)  # type: ignore[name-defined]
    else:
        print(">>> Vue frontend build complete (JS/CSS/HTML are gzip-compressed by Vite).")


# Hook into both so it works whether you call `buildfs` or `uploadfs` directly
env.AddPreAction("buildfs",  build_frontend)   # type: ignore[name-defined]
env.AddPreAction("uploadfs", build_frontend)   # type: ignore[name-defined]
