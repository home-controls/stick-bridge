"""Host tests for the USB serial baud rate math (main/serial_baud.c, written from docs/usb-serial-baud.md).

Builds serial_baud_harness.c with ASan + UBSan in WSL and runs it: the spec's expected CH340/CH341 words
and FTDI values, the CH34x tie and preference rules (FACT 0 for equal rates, then the larger N; FACT 1
never with N < 9; equally close rates -> the higher), clamping, the chip-version bit, a sweep of rates
cross-checked against an independent brute force, and the FTDI nearest-eighth rounding, special values
and cap.

  python test_serial_baud.py
"""
import subprocess
import sys
from pathlib import Path

HERE = Path(__file__).resolve().parent
MAIN = HERE.parent.parent / "main"
EXE = "/tmp/serial_baud_harness"


def wsl_path(p: Path) -> str:
    if sys.platform != "win32":  # Linux (CI): run_all.py puts linux/wsl in front, paths stay as they are
        return p.resolve().as_posix()
    s = str(p.resolve()).replace("\\", "/")
    return f"/mnt/{s[0].lower()}{s[2:]}"


def wsl(*args: str) -> subprocess.CompletedProcess:
    return subprocess.run(["wsl", "--", *args], capture_output=True, text=True, check=False)


def main() -> int:
    r = wsl("gcc", "-std=c11", "-O1", "-g", "-Wall", "-Wextra", "-Werror", "-fsanitize=address,undefined",
            "-fno-sanitize-recover=all", "-I", wsl_path(MAIN), wsl_path(HERE / "serial_baud_harness.c"),
            wsl_path(MAIN / "serial_baud.c"), "-o", EXE)
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
