"""Runs every host test here (test_*.py) and fails if any fails.

    python test/host/run_all.py

On Windows each test builds and runs its harness in WSL (gcc with ASan/UBSan, zlib). On Linux, as in
CI, the same commands run directly: linux/wsl stands in for the wsl command. Needs gcc and zlib's
headers (Debian/Ubuntu: build-essential zlib1g-dev).
"""
import os
import subprocess
import sys
import time
from pathlib import Path

HERE = Path(__file__).resolve().parent


def main() -> int:
    env = dict(os.environ)
    if sys.platform != "win32":
        env["PATH"] = str(HERE / "linux") + os.pathsep + env.get("PATH", "")
    failed = []
    for test in sorted(HERE.glob("test_*.py")):
        print(f"== {test.name}", flush=True)
        t = time.monotonic()
        if subprocess.run([sys.executable, str(test)], cwd=HERE, env=env).returncode:
            failed.append(test.name)
        print(f"   {test.name}: {'FAILED' if test.name in failed else 'ok'} ({time.monotonic() - t:.0f} s)", flush=True)
    print(f"{len(failed)} of {len(list(HERE.glob('test_*.py')))} host tests failed" + (f": {', '.join(failed)}" if failed else ""))
    return 1 if failed else 0


if __name__ == "__main__":
    sys.exit(main())
