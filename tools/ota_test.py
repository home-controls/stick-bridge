"""Breaks updates on purpose against the real bridge and checks it survives every one.

Runs a release server on this PC that serves a different bad release per scenario (bad manifests,
MD5/SHA-256/size mismatches, truncated, stalled, dripping and oversized downloads, wrong images,
a corrupt image whose hashes match, a power cut mid-download, firmware that crashes at boot or
has no network), reads the bridge's serial log for its verdict and checks it's still up afterwards.

The bridge must run an OTA test build (it installs anything newer from this server, no click):
  idf.py -B build-ota-base -D BRIDGE_TEST=1 -D BRIDGE_VER=1.0.12 \
         -D "BRIDGE_EXTRA_DEFAULTS=<abs>/test/ota/sdkconfig.loop" -D SDKCONFIG=<abs>/build-ota-base/sdkconfig build
  (crash.bin / nonet.bin: BRIDGE_VER 9.9.x and sdkconfig.loop;sdkconfig.crash / .nonet. The bridge reports a rollback once per version, so bump x between runs.)
test/ota/sdkconfig.loop holds this PC's address. Images dir: base.bin, crash.bin, nonet.bin, release-<v>.bin.

  python tools/ota_test.py <images dir> [--com COM3] [--only name,name] [--bridge 192.168.1.50]
"""

import argparse
import asyncio
import hashlib
import json
import os
import queue
import re
import sys
import threading
import time
import urllib.request
from pathlib import Path

import serial
from aiohttp import web

DESC = 24 + 8  # image header + first segment header, then esp_app_desc_t
VER_AT, PROJ_AT = DESC + 16, DESC + 48
BASE_VER = "1.0.12+test"


def patched(img: bytes, version=None, project=None, chip=None) -> bytes:
    b = bytearray(img)
    if version is not None:
        b[VER_AT:VER_AT + 32] = version.encode().ljust(32, b"\0")
    if project is not None:
        b[PROJ_AT:PROJ_AT + 32] = project.encode().ljust(32, b"\0")
    if chip is not None:
        b[12:14] = chip.to_bytes(2, "little")
    return bytes(b)


def image_version(img: bytes) -> str:
    return img[VER_AT:VER_AT + 32].split(b"\0")[0].decode()


def manifest(version, img, file, **over):
    m = {"version": version, "file": file, "size": len(img), "md5": hashlib.md5(img).hexdigest(),
         "sha256": hashlib.sha256(img).hexdigest(), "date": "2026-11-21", "notes": "OTA test"}
    m.update(over)
    return m


class Board:
    """Serial log reader (never resets the board unless asked)."""

    def __init__(self, port, logfile):
        self.s = serial.Serial()
        self.s.port, self.s.baudrate, self.s.timeout = port, 115200, 0.2
        self.s.dtr = self.s.rts = False
        self.s.open()
        self.q = queue.Queue()
        self.log = open(logfile, "a", encoding="utf-8")
        threading.Thread(target=self._read, daemon=True).start()

    def _read(self):
        buf = b""
        while True:
            buf += self.s.read(4096)
            while b"\n" in buf:
                line, buf = buf.split(b"\n", 1)
                t = line.decode("utf-8", "replace").rstrip()
                self.log.write(time.strftime("%H:%M:%S ") + t + "\n")
                self.log.flush()
                self.q.put(t)

    def drain(self):
        while not self.q.empty():
            self.q.get_nowait()

    def wait(self, patterns, timeout, bad=(r"Guru Meditation", r"abort\(\) was called(?!.*TEST)")):
        """Waits for all patterns in order. Returns (ok, lines seen, why)."""
        seen, todo, end = [], list(patterns), time.time() + timeout
        while todo and time.time() < end:
            try:
                t = self.q.get(timeout=0.5)
            except queue.Empty:
                continue
            seen.append(t)
            for b in bad:
                if re.search(b, t) and not self.expect_crash:
                    return False, seen, f"unexpected: {t}"
            if re.search(todo[0], t):
                todo.pop(0)
        return (not todo), seen, ("" if not todo else f"never saw /{todo[0]}/")

    expect_crash = False

    def reset(self):
        """EN low for 100 ms: a power cut as far as the chip is concerned."""
        self.s.rts = True
        time.sleep(0.1)
        self.s.rts = False


class Server:
    def __init__(self, images: Path, board: Board):
        self.board = board
        self.base = (images / "base.bin").read_bytes()
        self.crash = (images / "crash.bin").read_bytes()
        self.nonet = (images / "nonet.bin").read_bytes()
        rel = sorted(images.glob("release-*.bin"))[-1]
        self.release_ver = rel.stem.split("-", 1)[1]
        self.release = rel.read_bytes()
        self.cur = None  # current scenario dict
        self.hits = []

    # Each scenario: name, what the manifest request gets, what the image request gets,
    # the serial lines that mean "handled right", and how long to wait.
    def scenarios(self):
        base = self.base
        v = lambda n: f"9.9.{n}+test"
        img = lambda n: patched(base, version=v(n))
        S = []

        def add(name, expect, timeout=60, mf=None, image=None, **kw):
            S.append(dict(name=name, expect=expect, timeout=timeout, mf=mf, image=image, **kw))

        # --- the manifest itself
        add("manifest 404", [r"Update check failed: HTTP 404"], mf=("status", 404))
        add("manifest not JSON", [r"manifest is incomplete"], mf=("raw", b"<html>not json"))
        add("manifest without md5", [r"manifest is incomplete"], mf=("json", {k: x for k, x in manifest(v(1), img(1), "img/x.bin").items() if k != "md5"}))
        add("manifest md5 not lowercase hex", [r"manifest is incomplete"], mf=("json", manifest(v(1), img(1), "img/x.bin", md5="ABCDEF" + "0" * 26)))
        add("manifest size 10 MB", [r"manifest is incomplete"], mf=("json", manifest(v(1), img(1), "img/x.bin", size=10_000_000)))
        add("manifest version 40 chars", [r"manifest is incomplete"], mf=("json", manifest("9." * 20, img(1), "img/x.bin")))
        add("manifest server hangs", [r"Update check failed: no answer"], timeout=70, mf=("hang", 40))
        add("manifest redirect", [r"Release on git: 1\.0\.0 \(running [^)]+\)$"], mf=("redirect", "/real/manifest.json"),
            real=manifest("1.0.0", base, "img/x.bin"))
        add("manifest redirect loop", [r"Update check failed: too many redirects"], mf=("redirect", "/manifest.json"))
        add("manifest 6 KB notes, older", [r"Release on git: 1\.0\.1 \(running [^)]+\)$"], mf=("json", manifest("1.0.1", base, "img/x.bin", notes="n" * 6000)))
        # --- the download (manifest fine, image wrong)
        add("MD5 mismatch", [r"failed: MD5 doesn't match"], mf=("json", manifest(v(10), img(10), "img/i.bin", md5="0" * 32)), image=("ok", img(10)))
        add("SHA-256 mismatch", [r"failed: SHA-256 doesn't match"], mf=("json", manifest(v(11), img(11), "img/i.bin", sha256="0" * 64)), image=("ok", img(11)))
        add("size bigger than download", [r"size doesn't match the release"], mf=("json", manifest(v(12), img(12), "img/i.bin", size=len(base) + 4096)), image=("ok", img(12)))
        add("truncated (Content-Length)", [r"failed: the download stopped early"], mf=("json", manifest(v(13), img(13), "img/i.bin")), image=("cut", img(13)))
        add("truncated (chunked)", [r"failed: the download stopped early"], mf=("json", manifest(v(14), img(14), "img/i.bin")), image=("chunk_cut", img(14)))
        add("extra bytes (chunked)", [r"failed: the download is bigger than the release"], mf=("json", manifest(v(15), img(15), "img/i.bin")), image=("chunk_extra", img(15)))
        add("image 404", [r"failed: the download answered with an error"], mf=("json", manifest(v(16), img(16), "img/i.bin")), image=("status", 404))
        x = patched(base, version=v(17), chip=0)
        add("image for another chip", [r"failed: Firmware is for a different chip"], mf=("json", manifest(v(17), x, "img/i.bin")), image=("ok", x))
        x = patched(base, version=v(18), project="twisted_lamp")
        add("image of another project", [r"failed: Firmware is not a Zigbee Ethernet bridge image"], mf=("json", manifest(v(18), x, "img/i.bin")), image=("ok", x))
        x = img(99)
        add("image version != manifest", [r"failed: the image's version isn't the release's"], mf=("json", manifest(v(19), x, "img/i.bin")), image=("ok", x))
        x = os.urandom(300_000)
        add("not a firmware image", [r"failed: Not an ESP32 firmware image"], mf=("json", manifest(v(20), x, "img/i.bin")), image=("ok", x))
        x = img(21)  # version patched after the build: its own appended SHA-256 no longer matches
        add("corrupt image, hashes match", [r"failed: the image failed its own verification"], mf=("json", manifest(v(21), x, "img/i.bin")), image=("ok", x))
        add("server stalls mid-download", [r"failed: the download stopped early"], timeout=90, mf=("json", manifest(v(22), img(22), "img/i.bin")), image=("stall", img(22)))
        add("server drips 2 KB/s", [r"failed: the download took too long"], timeout=100, mf=("json", manifest(v(23), img(23), "img/i.bin")), image=("drip", img(23)))
        add("power cut mid-download", [r"App version: +" + re.escape(BASE_VER), r"Release on git"], timeout=90,
            mf=("json", manifest(v(26), img(26), "img/i.bin")), image=("powercut", img(26)), after_boot=True)
        # --- firmware that installs fine but doesn't work: must roll back by itself
        add("new firmware crashes at boot", [r"verified .* rebooting", r"crashing at boot on purpose", r"App version: +" + re.escape(BASE_VER),
            r"didn't start properly and was rolled back"], timeout=120, mf=("json", manifest(image_version(self.crash), self.crash, "img/i.bin")),
            image=("ok", self.crash), crash=True)
        add("new firmware has no network", [r"verified .* rebooting", r"no network on purpose", r"Trial failed after 120 s",
            r"App version: +" + re.escape(BASE_VER), r"didn't start properly and was rolled back"], timeout=260,
            mf=("json", manifest(image_version(self.nonet), self.nonet, "img/i.bin")), image=("ok", self.nonet))
        # --- and finally a good one
        add(f"good release {self.release_ver}", [r"verified .* rebooting", r"App version: +" + re.escape(self.release_ver), r"Firmware confirmed"],
            timeout=150, mf=("json", manifest(self.release_ver, self.release, "img/i.bin")), image=("ok", self.release), last=True)
        return S

    async def h_manifest(self, req):
        sc = self.cur
        self.hits.append(("manifest", req.path))
        if sc is None:
            return web.Response(status=404)
        if req.path.startswith("/real/"):
            return web.json_response(sc["real"])
        kind, arg = sc["mf"]
        if kind == "status":
            return web.Response(status=arg)
        if kind == "raw":
            return web.Response(body=arg, content_type="application/json")
        if kind == "json":
            return web.json_response(arg)
        if kind == "hang":
            await asyncio.sleep(arg)
            return web.Response(status=503)
        if kind == "redirect":
            raise web.HTTPFound(arg)

    async def h_image(self, req):
        sc = self.cur
        self.hits.append(("image", req.path))
        if sc is None or not sc["image"]:
            return web.Response(status=404)
        kind, arg = sc["image"]
        if kind == "status":
            return web.Response(status=arg)
        if kind == "ok":
            return web.Response(body=arg, content_type="application/octet-stream")
        r = web.StreamResponse()
        if kind in ("cut", "stall", "drip", "powercut"):
            r.content_length = len(arg)
        else:
            r.enable_chunked_encoding()
        await r.prepare(req)
        half = len(arg) // 2
        try:
            if kind in ("cut", "chunk_cut"):
                await r.write(arg[:half])
                req.transport.close()
                return r
            if kind == "chunk_extra":
                await r.write(arg + os.urandom(4096))
                await r.write_eof()
                return r
            if kind == "stall":
                await r.write(arg[:half])
                await asyncio.sleep(60)
                return r
            if kind == "drip":
                for i in range(0, len(arg), 1024):
                    await r.write(arg[i:i + 1024])
                    await asyncio.sleep(0.5)
                return r
            if kind == "powercut":
                for i in range(0, len(arg), 4096):
                    await r.write(arg[i:i + 4096])
                    if i > len(arg) * 0.4:
                        self.board.reset()
                        await asyncio.sleep(5)
                        req.transport.close()
                        return r
                    await asyncio.sleep(0.01)
        except (ConnectionResetError, asyncio.CancelledError):
            pass
        return r


def bridge_version(host):
    try:
        with urllib.request.urlopen(f"http://{host}/api/state", timeout=5) as f:
            return json.load(f).get("version")
    except Exception as e:  # noqa: BLE001
        return f"unreachable ({e.__class__.__name__})"


async def main():
    p = argparse.ArgumentParser()
    p.add_argument("images")
    p.add_argument("--com", default="COM3")
    p.add_argument("--bridge", default="192.168.1.50")
    p.add_argument("--port", type=int, default=8765)
    p.add_argument("--only", default="")
    a = p.parse_args()

    images = Path(a.images)
    board = Board(a.com, images / "serial.log")
    srv = Server(images, board)
    app = web.Application()
    app.router.add_get("/manifest.json", srv.h_manifest)
    app.router.add_get("/real/manifest.json", srv.h_manifest)
    app.router.add_get("/img/{name}", srv.h_image)
    runner = web.AppRunner(app)
    await runner.setup()
    await web.TCPSite(runner, "0.0.0.0", a.port).start()

    scen = srv.scenarios()
    if a.only:
        want = [w.strip().lower() for w in a.only.split(",")]
        scen = [s for s in scen if any(w in s["name"].lower() for w in want)]
    print(f"{len(scen)} scenarios; bridge runs {bridge_version(a.bridge)}", flush=True)
    results = []
    loop = asyncio.get_running_loop()
    for sc in scen:
        board.drain()
        board.expect_crash = bool(sc.get("crash"))
        srv.cur = sc
        t0 = time.time()
        ok, seen, why = await loop.run_in_executor(None, board.wait, sc["expect"], sc["timeout"])
        srv.cur = None
        # Still alive and on the expected firmware?
        want = srv.release_ver if sc.get("last") else BASE_VER
        ver = None
        for _ in range(30):
            ver = await loop.run_in_executor(None, bridge_version, a.bridge)
            if ver == want:
                break
            await asyncio.sleep(2)
        alive = ver == want
        res = "PASS" if ok and alive else "FAIL"
        detail = why or ("" if alive else f"bridge runs {ver}, expected {want}")
        results.append((sc["name"], res, round(time.time() - t0), detail))
        print(f"{res}  {sc['name']:<34} {round(time.time() - t0):>4} s  {detail}", flush=True)
        if not ok:
            for t in seen[-12:]:
                print("      | " + t, flush=True)
        # Let the next check come from a calm bridge (and keep resets > 10 s apart).
        await asyncio.sleep(3)
    print()
    print(f"{sum(r[1] == 'PASS' for r in results)}/{len(results)} passed")
    await runner.cleanup()


if __name__ == "__main__":
    asyncio.run(main())
