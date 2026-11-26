"""Host tests for the streaming single-file unzip (main/unzip_core.c), built in WSL with ASan/UBSan and zlib
as the inflater: a real Koenkk Z-Stack release zip (fetched if the network is there) must unpack byte-for-byte
like Python's zipfile, in random-sized pieces; stored entries, damaged data, a wrong CRC, a truncated file,
encryption and non-zip input are refused.

  python test_unzip.py [release.zip]
"""
import io
import random
import subprocess
import sys
import tempfile
import urllib.request
import zipfile
from pathlib import Path

HERE = Path(__file__).resolve().parent
MAIN = HERE.parent.parent / "main"
EXE = "/tmp/unzip_harness"
REAL = ("https://github.com/Koenkk/Z-Stack-firmware/releases/download/Z-Stack_3.x.0_coordinator_20250321/"
        "CC1352P2_CC2652P_launchpad_coordinator_20250321.zip")
FAILS = []


def wsl_path(p: Path) -> str:
    s = str(p.resolve()).replace("\\", "/")
    return f"/mnt/{s[0].lower()}{s[2:]}"


def run(path: Path, seed: int) -> subprocess.CompletedProcess:
    return subprocess.run(["wsl", "--", EXE, wsl_path(path), str(seed)], capture_output=True, check=False)


def check(name, cond, info=""):
    print(("ok   " if cond else "FAIL ") + name + ("" if cond else f"  -- {info}"))
    if not cond:
        FAILS.append(name)


def main() -> int:
    r = subprocess.run(["wsl", "--", "gcc", "-std=c11", "-O1", "-g", "-Wall", "-Wextra", "-Werror", "-fsanitize=address,undefined",
                        "-fno-sanitize-recover=all", "-I", wsl_path(MAIN), wsl_path(HERE / "unzip_harness.c"),
                        wsl_path(MAIN / "unzip_core.c"), "-lz", "-o", EXE], capture_output=True, text=True, check=False)
    if r.returncode:
        print(r.stdout, r.stderr)
        return 1
    tmp = tempfile.TemporaryDirectory()
    t = Path(tmp.name)
    rnd = random.Random(5)
    payload = bytes(rnd.getrandbits(8) for _ in range(150000)) + b":10000000ABCD\n" * 5000

    def mk(name, method, data=payload):
        b = io.BytesIO()
        with zipfile.ZipFile(b, "w", compression=method) as z:
            z.writestr("fw.hex", data)
        p = t / name
        p.write_bytes(b.getvalue())
        return p

    defl, stored = mk("d.zip", zipfile.ZIP_DEFLATED), mk("s.zip", zipfile.ZIP_STORED)
    for name, p in (("deflate", defl), ("stored", stored)):
        for seed in (1, 2, 3):
            r = run(p, seed)
            check(f"{name} seed {seed}", r.returncode == 0 and r.stdout == payload, r.stderr.decode()[-200:])
    # damage: flip a byte in the compressed data / the CRC field / cut it short / not a zip / encrypted flag
    d = bytearray(defl.read_bytes())
    bad = t / "bad.zip"
    d2 = bytearray(d); d2[2000] ^= 0xFF; bad.write_bytes(d2)
    r = run(bad, 1)
    check("damaged deflate data refused", r.returncode == 1 and (b"damaged" in r.stderr or b"CRC" in r.stderr or b"size" in r.stderr), r.stderr)
    d3 = bytearray(d); d3[14] ^= 1; bad.write_bytes(d3)
    r = run(bad, 1)
    check("wrong CRC-32 refused", r.returncode == 1 and b"CRC-32" in r.stderr, r.stderr)
    bad.write_bytes(bytes(d[: len(d) // 2]))
    r = run(bad, 1)
    check("truncated refused", r.returncode == 1 and b"early" in r.stderr, r.stderr)
    bad.write_bytes(b"PK\x05\x06" + b"\0" * 40)
    r = run(bad, 1)
    check("not a zip refused", r.returncode == 1 and b"not a zip" in r.stderr, r.stderr)
    d4 = bytearray(d); d4[6] |= 1; bad.write_bytes(d4)
    r = run(bad, 1)
    check("encrypted refused", r.returncode == 1 and b"encrypted" in r.stderr, r.stderr)
    # real release
    real = Path(sys.argv[1]) if len(sys.argv) > 1 else None
    if real is None:
        try:
            req = urllib.request.Request(REAL, headers={"User-Agent": "zigbee-eth-bridge-test"})
            real = t / "real.zip"
            real.write_bytes(urllib.request.urlopen(req, timeout=60).read())
        except OSError as e:
            print(f"skip real release ({e})")
            real = None
    if real:
        want = zipfile.ZipFile(real).read(zipfile.ZipFile(real).infolist()[0].filename)
        for seed in (1, 2):
            r = run(real, seed)
            check(f"real Z-Stack zip seed {seed} ({len(want)} bytes)", r.returncode == 0 and r.stdout == want, r.stderr.decode()[-200:])
    print("all passed" if not FAILS else f"FAILED: {len(FAILS)}")
    return 1 if FAILS else 0


if __name__ == "__main__":
    sys.exit(main())
