"""Host tests for USB OUT pacing and XON/XOFF holds (main/txpace_core.c).

Builds txpace_harness.c with ASan + UBSan in WSL and runs its scenarios on a virtual clock: lone frames
go out at once, bursts are cut at ASH frame boundaries into <= 64-byte chunks with idle line between
them, XOFF holds the next chunk until XON (or gives up after xoff_max / the write timeout), and
nothing is lost or reordered.

  python test_txpace.py
"""
import subprocess
import sys
from pathlib import Path

HERE = Path(__file__).resolve().parent
MAIN = HERE.parent.parent / "main"
EXE = "/tmp/txpace_harness"


def wsl_path(p: Path) -> str:
    if sys.platform != "win32":  # Linux (CI): run_all.py puts linux/wsl in front, paths stay as they are
        return p.resolve().as_posix()
    s = str(p.resolve()).replace("\\", "/")
    return f"/mnt/{s[0].lower()}{s[2:]}"


def wsl(*args: str) -> subprocess.CompletedProcess:
    return subprocess.run(["wsl", "--", *args], capture_output=True, text=True, check=False)


def main() -> int:
    r = wsl("gcc", "-std=c11", "-O1", "-g", "-Wall", "-Wextra", "-Werror", "-fsanitize=address,undefined",
            "-fno-sanitize-recover=all", "-I", wsl_path(MAIN), wsl_path(HERE / "txpace_harness.c"),
            wsl_path(MAIN / "txpace_core.c"), "-o", EXE)
    if r.returncode:
        print(r.stdout, r.stderr)
        return 1
    r = wsl(EXE, "selftest")
    print(r.stdout.strip())
    if r.returncode:
        print(r.stderr)
    return r.returncode


if __name__ == "__main__":
    sys.exit(main())
