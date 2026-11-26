"""Host tests for the stored log's flash format (main/logstore_core.c).

Builds logstore_harness.c with ASan + UBSan in WSL and runs its scenarios on a simulated NOR flash
(blank and garbage areas, wrapping, remounts, power cuts mid-write and mid-erase, clear + floor,
a reader lapped by the writer), then checks the line stamps against Python's own UTC dates.

  python test_logstore.py
"""
import random
import subprocess
import sys
from datetime import datetime, timezone
from pathlib import Path

HERE = Path(__file__).resolve().parent
MAIN = HERE.parent.parent / "main"
EXE = "/tmp/logstore_harness"


def wsl_path(p: Path) -> str:
    if sys.platform != "win32":  # Linux (CI): run_all.py puts linux/wsl in front, paths stay as they are
        return p.resolve().as_posix()
    s = str(p.resolve()).replace("\\", "/")
    return f"/mnt/{s[0].lower()}{s[2:]}"


def wsl(*args: str, check=True) -> subprocess.CompletedProcess:
    return subprocess.run(["wsl", "--", *args], capture_output=True, text=True, check=check)


def build() -> None:
    r = wsl("gcc", "-std=c11", "-O1", "-g", "-Wall", "-Wextra", "-Werror", "-fsanitize=address,undefined",
            "-fno-sanitize-recover=all", "-I", wsl_path(MAIN), wsl_path(HERE / "logstore_harness.c"),
            wsl_path(MAIN / "logstore_core.c"), "-o", EXE, check=False)
    if r.returncode:
        print(r.stdout, r.stderr)
        sys.exit("build failed")


def main() -> int:
    build()
    r = wsl(EXE, "selftest", check=False)
    print(r.stdout.strip().splitlines()[-1] if r.stdout.strip() else "(no output)")
    if r.returncode:
        print(r.stdout, r.stderr)
        return 1
    rng = random.Random(7)
    bad = 0
    for _ in range(300):
        boot = rng.randrange(1_600_000_000_000, 4_000_000_000_000)
        ms = rng.randrange(0, 2**32 - 1)
        line = f"I ({ms}) t: x"
        out = wsl(EXE, "stamp", str(boot), line).stdout
        t = boot + ms
        want = datetime.fromtimestamp(t // 1000, timezone.utc).strftime("%Y-%m-%dT%H:%M:%S") + f".{t % 1000:03d}Z {line}\n"
        if out != want:
            bad += 1
            print("stamp mismatch:", repr(out), repr(want))
        back = int(wsl(EXE, "parse", want[:24]).stdout)
        if back != t:
            bad += 1
            print("parse mismatch:", want[:24], back, t)
    print(f"{'ok' if not bad else 'FAIL'} 600 date checks against Python, {bad} failed")
    return 1 if bad else 0


if __name__ == "__main__":
    sys.exit(main())
