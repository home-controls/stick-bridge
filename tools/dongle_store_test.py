"""On-device check of the dongle image store (main/ncpflash.c): a test build (test/dongle/sdkconfig.store) pulls
/1.gbl .. /6.gbl from this server at boot through the same upload path the web page uses, logs each result on
the serial console, and this script checks them; then it resets the board and checks the stored image is
still there.

  1 bad CRC-32            -> refused          4 announced 3 MB        -> refused (too big)
  2 router firmware       -> stored, flagged  5 an ESP32 image        -> refused (not a GBL)
  3 cut off mid-transfer  -> refused          6 a real NCP .gbl       -> stored; survives a reset
  then: flash with the wrong MD5 -> refused, flash with no dongle on J3 -> refused

  python tools/dongle_store_test.py <real ncp .gbl> [COM3] [port 8766]
"""
import asyncio
import re
import sys
import threading
import time
from pathlib import Path

import serial
from aiohttp import web

sys.path.insert(0, str(Path(__file__).resolve().parent.parent / "test" / "host"))
from test_ncpflash import gbl  # noqa: E402

REAL = Path(sys.argv[1]).read_bytes()
PORT_NAME = sys.argv[2] if len(sys.argv) > 2 else "COM3"
HTTP_PORT = int(sys.argv[3]) if len(sys.argv) > 3 else 8766
LINES: list[str] = []


def reader(stop: threading.Event):
    s = serial.Serial()
    s.port, s.baudrate, s.timeout, s.dtr, s.rts = PORT_NAME, 115200, 0.3, False, False
    s.open()
    buf = b""
    while not stop.is_set():
        buf += s.read(4096)
        while b"\n" in buf:
            line, buf = buf.split(b"\n", 1)
            t = line.decode("utf-8", "replace").rstrip()
            LINES.append(t)
            if "dongle_fw" in t or "TEST" in t:
                print("   |", t[:220], flush=True)
        if RESET.is_set():
            RESET.clear()
            s.rts = True  # EN low: a reset (counts as a power-on: keep these >10 s apart)
            time.sleep(0.1)
            s.rts = False
    s.close()


RESET = threading.Event()


async def serve(r: web.Request):
    n = r.match_info["n"]
    if n == "1":
        return web.Response(body=gbl(crc_ok=False))
    if n == "2":
        return web.Response(body=gbl(meta='{"metadata_version":2,"fw_type":"zigbee_router","baudrate":115200}'))
    if n == "3":
        body = gbl(prog=200000)
        resp = web.StreamResponse(headers={"Content-Length": str(len(body))})
        await resp.prepare(r)
        await resp.write(body[:60000])
        await asyncio.sleep(0.5)
        r.transport.close()  # cut off
        return resp
    if n == "4":
        resp = web.StreamResponse(headers={"Content-Length": str(3 * 1024 * 1024)})
        await resp.prepare(r)
        try:
            await resp.write(b"\xeb\x17\xa6\x03" + bytes(4092))
            await asyncio.sleep(3)
        except Exception:
            pass
        return resp
    if n == "5":
        return web.Response(body=bytes([0xE9, 3, 2, 0x20]) + bytes(100000))
    if n == "6":
        return web.Response(body=REAL)
    return web.Response(status=404)


def wait_for(pattern: str, timeout: float, start: int = 0) -> str | None:
    t0 = time.time()
    while time.time() - t0 < timeout:
        for line in LINES[start:]:
            if re.search(pattern, line):
                return line
        time.sleep(0.2)
    return None


FAILS = []


def check(name, line, want):
    ok = line is not None and re.search(want, line) is not None
    print(("ok   " if ok else "FAIL ") + name + ("" if ok else f"  -- {line}"))
    if not ok:
        FAILS.append(name)


async def main():
    app = web.Application()
    app.router.add_get("/{n}.gbl", serve)
    runner = web.AppRunner(app)
    await runner.setup()
    await web.TCPSite(runner, "0.0.0.0", HTTP_PORT).start()
    stop = threading.Event()
    th = threading.Thread(target=reader, args=(stop,), daemon=True)
    th.start()
    print(f"serving on :{HTTP_PORT}; resetting the board")
    await asyncio.sleep(1)
    RESET.set()
    loop = asyncio.get_running_loop()
    done = await loop.run_in_executor(None, wait_for, r"TEST done", 120)
    if not done:
        print("never saw 'TEST done'")
        FAILS.append("finished")
    res = {n: next((l for l in LINES if f"TEST dongle image {n}:" in l), None) for n in range(1, 7)}
    js = {n: next((l for l in LINES if f"TEST json {n}:" in l), None) for n in range(1, 7)}
    check("area registered on the old partition table", next((l for l in LINES if "Dongle image area at" in l), None), r"0x820000, 2048 KB")
    check("1 bad CRC-32 refused", res[1], r"CRC-32")
    check("1 leaves no image", js[1], r'"image":null')
    check("2 router firmware stored", res[2], r": stored")
    check("2 flagged as not a coordinator", js[2], r'"problem":"This is zigbee_router')
    check("3 cut off mid-transfer refused", res[3], r"cut short")
    check("3 leaves no half image", js[3], r'"image":null')
    check("4 3 MB refused", res[4], r"too big")
    check("5 ESP32 image refused", res[5], r"Not a GBL")
    check("6 real NCP stored", res[6], r": stored")
    check("6 metadata read", js[6], r'"name":"6.gbl".*"fw_type":"zigbee_ncp".*"version":"8\.0\.3\.0"')
    check("flash with the wrong MD5 refused", next((l for l in LINES if "TEST flash with the wrong MD5" in l), None), r"isn't the stored image")
    check("flash with no dongle refused", next((l for l in LINES if "TEST flash with no dongle" in l), None), r"No dongle connected")
    check("no errors from the store", "\n".join(l for l in LINES if l.startswith("E (") and "dongle_fw" in l) or "none", r"^none$")
    print("waiting 12 s before the next reset (power-cycle counter)")
    await asyncio.sleep(12)
    mark = len(LINES)
    RESET.set()
    await loop.run_in_executor(None, wait_for, r"TEST cleaned up", 30, mark)
    check("image survives a reset", next((l for l in LINES[mark:] if "TEST persisted" in l), None), r"TEST persisted: 6\.gbl")
    check("checked again at boot", next((l for l in LINES[mark:] if "Stored dongle image" in l), None), r"Stored dongle image: 6\.gbl")
    check("test image removed", next((l for l in LINES[mark:] if "TEST cleaned up" in l), None), r"image removed")
    stop.set()
    await runner.cleanup()
    print(f"\n{len(FAILS)} failed" if FAILS else "\nall passed")
    sys.exit(1 if FAILS else 0)


asyncio.run(main())
