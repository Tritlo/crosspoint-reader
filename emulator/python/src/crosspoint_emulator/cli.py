from __future__ import annotations

import argparse
import os
import runpy
import sys
from pathlib import Path

from .client import EXECUTABLE_ENVIRONMENT_VARIABLE, TIMING_PROFILE_ENVIRONMENT_VARIABLE


def _set_override(name: str, value: Path | None) -> None:
    if value is not None:
        os.environ[name] = str(value.resolve())


def main() -> None:
    parser = argparse.ArgumentParser(description="Run a Python script with the CrossPoint emulator client installed")
    parser.add_argument("--executable", type=Path, help="native emulator runner")
    parser.add_argument("--timing-profile", type=Path, help="override the bundled device timing profile")
    parser.add_argument("script", type=Path, help="ordinary Python script to execute")
    parser.add_argument("arguments", nargs=argparse.REMAINDER, help="arguments passed to the script")
    options = parser.parse_args()

    script = options.script.resolve()
    if not script.is_file():
        parser.error(f"script not found: {script}")

    _set_override(EXECUTABLE_ENVIRONMENT_VARIABLE, options.executable)
    _set_override(TIMING_PROFILE_ENVIRONMENT_VARIABLE, options.timing_profile)
    sys.argv = [str(script), *options.arguments]
    sys.path.insert(0, str(script.parent))
    runpy.run_path(str(script), run_name="__main__")


if __name__ == "__main__":
    main()
