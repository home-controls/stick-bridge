"""Host tests for the USB-serial arithmetic, the dongle catalogue and protocol detection
(main/usbser_core.c, dongles.c, protodetect.c), built in WSL with ASan + UBSan: chip by VID:PID,
CH340 and FTDI baud divisors (Linux values), FTDI status-byte stripping, catalogue matches for real
USB strings, and detection of ASH, ZNP, deCONZ, ZiGate and Spinel frames (not random noise).

  python test_usbser.py
"""
import subprocess
import sys
from pathlib import Path

HERE = Path(__file__).resolve().parent
MAIN = HERE.parent.parent / "main"
EXE = "/tmp/usbser_harness"


def wsl_path(p: Path) -> str:
    s = str(p.resolve()).replace("\\", "/")
    return f"/mnt/{s[0].lower()}{s[2:]}"


def wsl(*args: str) -> subprocess.CompletedProcess:
    return subprocess.run(["wsl", "--", *args], capture_output=True, text=True, check=False)


def main() -> int:
    r = wsl("gcc", "-std=c11", "-O1", "-g", "-Wall", "-Wextra", "-Werror", "-fsanitize=address,undefined",
            "-fno-sanitize-recover=all", "-I", wsl_path(MAIN), wsl_path(HERE / "usbser_harness.c"),
            wsl_path(MAIN / "usbser_core.c"), wsl_path(MAIN / "dongles.c"), wsl_path(MAIN / "protodetect.c"), "-o", EXE)
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
