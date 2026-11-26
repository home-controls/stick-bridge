"""Drives the firmware's own ConBee flasher (main/gcfflash_core.c, built for the host by gcfflash_harness.c with
ASan + UBSan in WSL) against simulated dresden elektronik sticks on a virtual clock:

- deCONZ firmware: SLIP framing with the 16-bit two's-complement sum, VERSION, WRITE_PARAMETER watchdog
  timeout (0x26) -> reset 2 s later, optional unsolicited frames.
- ConBee II (SAMR21, USB CDC): every reset drops the USB port and it comes back after a while (the host must
  reopen); V1 bootloader: "ID" -> banner, sync 1A 1C A9 AE -> READY, 10-byte header, GET pages, #VALID CRC.
- ConBee III (MG21 behind FTDI): the port survives resets; V3 bootloader: 0x81 SLIP commands ID / FW_UPDATE /
  data requests, its verdict as an ID response, then the app starts.
- Faults: wrong files, damaged files, wrong firmware platform on the stick, stick that never enters (or
  falls back out of) its bootloader, never re-enumerates, page / data requests out of range, lost and corrupted
  bytes, commit that never comes / comes late / is cut by the reset, refused update, bad image, silent or wrong
  new firmware.

Plus the GCF checker on synthetic files, and on the real published files when the network allows.

  uv run --python 3.12.15 --no-project test_gcfflash.py [real.GCF ...]
"""
import json
import random
import struct
import subprocess
import sys
import tempfile
import urllib.request
import zlib
from pathlib import Path

HERE = Path(__file__).resolve().parent
MAIN = HERE.parent.parent / "main"
EXE = "/tmp/gcfflash_harness"
BYTE_MS = 10000.0 / 115200

NAME2 = "deCONZ_ConBeeII_0x26780700.bin.GCF"
NAME3 = "deCONZ_ConBeeIII_0x26510900.bin.GCF"
OLD2, NEW2 = 0x26720700, 0x26780700
OLD3, NEW3 = 0x26500900, 0x26510900
BTL3 = 0x00010203
SYNC = bytes([0x1A, 0x1C, 0xA9, 0xAE])
BANNER = b"\r\ndeCONZ ConBee II Bootloader v1.07 (simulated)\r\n"
FIRMWARE_URL = "https://deconz.dresden-elektronik.de/deconz-firmware/"


# ---------------------------------------------------------------- GCF files (independent of the C code)

def crc8(data: bytes, crc=0) -> int:  # gcfflasher CRC8_Dallas
    for b in data:
        crc ^= b
        for _ in range(8):
            crc = ((crc << 1) ^ 0x31) & 0xFF if crc & 0x80 else (crc << 1) & 0xFF
    return crc


def rnd_bytes(n, seed):
    r = random.Random(seed)
    return bytes(r.getrandbits(8) for _ in range(n))


def gcf_v1(size=40000, *, seed=1, addr=0x5000, ftype=1) -> bytes:
    data = rnd_bytes(size, seed)  # encrypted firmware: its CRC-8 covers the plaintext, any value will do
    return struct.pack("<IBIIB", 0xCAFEFEED, ftype, addr, len(data), seed & 0xFF) + data


def container(plain=50000, *, seed=2, product=0xDEC0DE03, bad_trailer=False) -> tuple[bytes, int]:
    data = rnd_bytes(plain + (-plain) % 4, seed)
    plain_crc = zlib.crc32(rnd_bytes(plain, seed + 100))
    img = struct.pack("<IIIII", 20 + len(data), 61, 0x4000, plain, plain_crc) + data
    total = 8 + len(img) + 4
    body = struct.pack("<II", product, total) + img
    trailer = zlib.crc32(body) ^ (1 if bad_trailer else 0)
    return body + struct.pack("<I", trailer), plain_crc


def gcf_v3(plain=50000, *, seed=2, product=0xDEC0DE03, bad_crc8=False, bad_trailer=False) -> bytes:
    c, _ = container(plain, seed=seed, product=product, bad_trailer=bad_trailer)
    return struct.pack("<IBIIB", 0xCAFEFEED, 60, 0x4000, len(c), crc8(c) ^ (1 if bad_crc8 else 0)) + c


def gcf_v90(size=30000, seed=3) -> bytes:
    data = rnd_bytes(size, seed)
    return struct.pack("<IBIIBI", 0xCAFEFEED, 90, 0x4000, len(data), 0, 0x11223344) + data


def parse(img: bytes) -> dict:
    magic, ftype, addr, size, c8 = struct.unpack_from("<IBIIB", img)
    off, c32 = 14, 0
    if ftype == 60:
        c32 = struct.unpack_from("<I", img, 14 + 24)[0]
    elif ftype == 90:
        c32, off = struct.unpack_from("<I", img, 14)[0], 18
    return dict(type=ftype, target=addr, size=size, crc8=c8, crc32=c32, off=off, payload=img[off:off + size])


# ---------------------------------------------------------------- SLIP (independent of the C code)

def slip(data: bytes) -> bytes:
    crc = (-sum(data)) & 0xFFFF
    out = bytearray([0xC0])
    for b in data + struct.pack("<H", crc):
        out += b"\xdb\xdc" if b == 0xC0 else b"\xdb\xdd" if b == 0xDB else bytes([b])
    return bytes(out + b"\xc0")


class SlipRx:
    def __init__(self):
        self.buf, self.esc = bytearray(), False

    def feed(self, b):
        """Returns (frame, crc_ok) when b ends one, else None."""
        if b == 0xC0:
            out = None
            if not self.esc and len(self.buf) > 2:
                body, crc = bytes(self.buf[:-2]), struct.unpack("<H", self.buf[-2:])[0]
                out = (body, ((-sum(body)) & 0xFFFF) == crc)
            self.buf, self.esc = bytearray(), False
            return out
        if b == 0xDB:
            self.esc = True
            return None
        if self.esc:
            self.esc = False
            b = {0xDC: 0xC0, 0xDD: 0xDB}.get(b, b)
        self.buf.append(b)
        if len(self.buf) > 1024:
            self.buf = bytearray()
        return None


# ---------------------------------------------------------------- the simulated stick

class Stick:
    def __init__(self, kind, image, name, *, start_in="app", old=None, new=None, app_platform=None, wdog_works=True,
                 reset_to_app=False, enum_ms=1500, announce=False, chatter=False, new_answers=True,
                 # V1
                 drop_page=None, drop_bytes=10, lose_get=None, oob_at=None, oob_always=False, commit_ms=8000,
                 never_valid=False, reset_before_valid=False,
                 # V3
                 chunk=256, refuse=False, corrupt_resp=None, drop_req=None, v3_oob_at=None, bad_image=False, verify_ms=2000):
        self.kind = kind
        self.f = parse(image)
        self.now = 0.0
        self.out = []
        self.events = []
        self.evseq = 0
        self.port_open = True
        self.enum_at = None
        self.host_baud = None
        self.writes = 0
        self.wdog_count = 0
        self.resets = 0
        self.id_asked = 0
        self.t_update = None
        self.log = []
        self.old = old if old is not None else (OLD2 if kind == 2 else OLD3)
        self.new = new if new is not None else (NEW2 if kind == 2 else NEW3)
        if app_platform is not None:
            self.old = (self.old & ~0xFF00) | (app_platform << 8)
        self.version = self.old
        self.flashed = False
        self.app_crc = 0x0BADC0DE
        o = dict(wdog_works=wdog_works, reset_to_app=reset_to_app, enum_ms=enum_ms, announce=announce, chatter=chatter,
                 new_answers=new_answers, drop_page=drop_page, drop_bytes=drop_bytes, lose_get=lose_get, oob_at=oob_at,
                 oob_always=oob_always, commit_ms=commit_ms, never_valid=never_valid, reset_before_valid=reset_before_valid,
                 chunk=chunk, refuse=refuse, corrupt_resp=corrupt_resp, drop_req=drop_req, v3_oob_at=v3_oob_at,
                 bad_image=bad_image, verify_ms=verify_ms)
        self.__dict__.update(o)
        self.received = bytearray(len(self.f["payload"]))
        self.pages_got = set()
        self.done_once = set()
        self.token = 0
        self.mode = None
        self.boot(start_in)

    # ---- time and delivery
    def at(self, dt, fn):
        self.evseq += 1
        self.events.append((self.now + dt, self.evseq, fn))
        self.events.sort(key=lambda e: (e[0], e[1]))

    def run_events(self, upto):
        while self.events and self.events[0][0] <= upto:
            t, _, fn = self.events.pop(0)
            self.now = max(self.now, t)
            fn()

    def emit(self, data: bytes, delay=0.5):
        if not self.port_open:
            return  # nobody has the CDC port open: the bytes go nowhere
        t = max(self.now + delay, self.out[-1][0] if self.out else 0)
        for b in data:
            t += BYTE_MS
            self.out.append((t, b))

    def read(self, mx, timeout):
        if not self.port_open:
            self.now += 1
            return -1
        deadline = self.now + timeout
        while True:
            self.run_events(self.now)
            if not self.port_open:
                return -1
            got = bytearray()
            while self.out and self.out[0][0] <= self.now and len(got) < mx:
                got.append(self.out.pop(0)[1])
            if got:
                if self.kind == 3 and self.host_baud != 115200:
                    continue
                return bytes(got)
            if self.now >= deadline:
                return b""
            self.now = min([deadline] + ([self.out[0][0]] if self.out else []) + ([self.events[0][0]] if self.events else []))

    def write(self, data: bytes):
        if not self.port_open:
            return -1
        self.writes += 1
        self.now += len(data) * BYTE_MS
        self.run_events(self.now)
        if not self.port_open:
            return -1
        if self.kind == 3 and self.host_baud != 115200:
            return 0
        if self.mode == "app":
            for b in data:
                fr = self.arx.feed(b)
                if fr and fr[1]:
                    self.app_frame(fr[0])
        elif self.mode == "bl" and self.kind == 2:
            self.v1_bytes(data)
        elif self.mode == "bl":
            for b in data:
                fr = self.brx.feed(b)
                if fr:
                    self.v3_frame(*fr)
        return 0

    def reopen(self, timeout):
        if self.port_open:
            return 0
        if self.enum_at is None or self.enum_at > self.now + timeout:
            self.run_events(self.now + timeout)
            self.now += timeout
            return -1
        self.run_events(max(self.now, self.enum_at))
        self.now = max(self.now, self.enum_at) + 50
        self.port_open = True
        self.out.clear()
        self.log.append(f"reopened at {self.now:.0f}")
        return 0

    # ---- resets
    def reset(self, to):
        self.mode = "reset"
        self.resets += 1
        self.token += 1
        if self.kind == 2:
            self.port_open = False
            self.out.clear()
            self.enum_at = self.now + self.enum_ms if self.enum_ms is not None else None
        self.at(300, lambda: self.boot(to))

    def boot(self, to):
        self.mode = to
        self.arx, self.brx = SlipRx(), SlipRx()
        self.log.append(f"{self.now:.0f} boot {to}")
        if to == "app":
            if self.flashed:
                self.version = self.new
            if self.chatter:
                self.at(100, self.chat)
        elif self.kind == 2:
            self.v1, self.tail = "idle", b""
        else:
            self.v3 = "idle"
            if self.announce:
                self.emit(slip(struct.pack("<BBII", 0x81, 0x82, BTL3, self.app_crc)), delay=20)

    def chat(self):
        if self.mode == "app":
            self.emit(slip(bytes([0x0E, 0x10, 0x00, 0x07, 0x00, 0xA2, 0x00])))
            self.at(300, self.chat)

    # ---- deCONZ firmware
    def app_frame(self, f):
        if self.flashed and not self.new_answers:
            return
        if f[0] == 0x0D:
            self.emit(slip(struct.pack("<BBBHI", 0x0D, f[1], 0, 9, self.version)), delay=3)
        elif f[0] == 0x0B and len(f) >= 12 and f[7] == 0x26:
            self.wdog_count += 1
            self.emit(slip(bytes([0x0B, f[1], 0x00, 0x08, 0x00, 0x01, 0x00, 0x26])), delay=3)
            if self.wdog_works:
                self.at(2000, lambda: self.reset("bl"))
            elif self.reset_to_app:
                self.at(2000, lambda: self.reset("app"))

    # ---- V1 bootloader (ConBee II)
    def v1_bytes(self, data):
        for b in data:
            if self.v1 == "idle":
                self.tail = (self.tail + bytes([b]))[-4:]
                if self.tail == SYNC:
                    self.emit(b"READY\r\n")
                    self.v1, self.hbuf, self.tail = "header", bytearray(), b""
                elif self.tail[-2:] == b"ID":
                    self.id_asked += 1
                    self.emit(BANNER, delay=5)
                    self.tail = b""
            elif self.v1 == "header":
                self.hbuf.append(b)
                if len(self.hbuf) == 10:
                    size, addr, ftype, c8 = struct.unpack("<IIBB", self.hbuf)
                    f = self.f
                    if (size, addr, ftype, c8) == (f["size"], f["target"], f["type"], f["crc8"]):
                        self.pages = (size + 255) // 256
                        self.get(0, fresh=True)
                    else:
                        self.log.append(f"bad header {bytes(self.hbuf).hex()}")
                        self.v1 = "idle"
            elif self.v1 == "page":
                if self.dropping:
                    self.dropping -= 1
                    continue
                self.pbuf.append(b)
                if len(self.pbuf) == self.expect_n:
                    self.page_done()

    def get(self, page, fresh=False):
        req = page
        if fresh and self.oob_at == page and (self.oob_always or "oob" not in self.done_once):
            self.done_once.add("oob")
            req = self.pages + 5
        self.v1, self.cur, self.req_page, self.pbuf, self.dropping = "page", page, req, bytearray(), 0
        if fresh:
            self.tries = 0
        n = 256 if req >= self.pages else min(256, self.f["size"] - req * 256)
        self.expect_n = n
        msg = b"GET" + struct.pack("<H", req) + b";"
        if self.lose_get == page and "lose_get" not in self.done_once:
            self.done_once.add("lose_get")
            msg = msg[:1] + msg[2:]
        elif self.drop_page == page and "drop" not in self.done_once:
            self.done_once.add("drop")
            self.dropping = self.drop_bytes
        self.emit(msg)
        self.token += 1
        tok = self.token
        self.at(300, lambda: self.page_timeout(tok))

    def page_timeout(self, tok):
        if tok != self.token or self.mode != "bl" or self.v1 != "page":
            return
        self.tries += 1
        if self.tries > 5:
            self.v1, self.tail = "idle", b""
            return
        self.get(self.req_page if self.req_page < self.pages else self.cur)

    def page_done(self):
        p = self.req_page
        self.received[p * 256:p * 256 + len(self.pbuf)] = self.pbuf
        self.pages_got.add(p)
        self.token += 1
        if p + 1 >= self.pages:
            self.v1 = "commit"
            self.at(self.commit_ms, self.commit)
        else:
            self.get(p + 1, fresh=True)

    def commit(self):
        if self.mode != "bl" or self.v1 != "commit" or self.never_valid:
            return
        if bytes(self.received) != self.f["payload"]:
            self.log.append("image doesn't match")
            return
        self.flashed = True
        self.v1 = "done"
        if self.reset_before_valid:
            self.reset("app")
        else:
            self.emit(b"#VALID CRC\r\n")
            self.at(200, lambda: self.reset("app"))

    # ---- V3 bootloader (ConBee III)
    def bsend(self, payload, corrupt=False):
        fr = slip(payload)
        if corrupt:
            fr = fr[:3] + fr[4:]
        self.emit(fr, delay=2)

    def v3_frame(self, f, crc_ok):
        if len(f) >= 2 and f[0] == 0x81 and f[1] == 0x84:
            self.resp_count = getattr(self, "resp_count", 0) + 1
            if self.corrupt_resp is not None and self.resp_count == self.corrupt_resp:
                crc_ok = False  # hit on the wire
        if not crc_ok or len(f) < 2 or f[0] != 0x81:
            return
        if f[1] == 0x02:
            self.bsend(struct.pack("<BBII", 0x81, 0x82, BTL3, self.app_crc))
        elif f[1] == 0x03 and len(f) == 15:
            size, addr, ftype, c32 = struct.unpack("<IIBI", f[2:15])
            ok = (size, addr, ftype, c32) == (self.f["size"], self.f["target"], self.f["type"], self.f["crc32"]) and not self.refuse
            self.t_update = self.now
            self.bsend(bytes([0x81, 0x83, 0 if ok else 1]))
            if ok:
                self.v3, self.off = "upload", 0
                self.at(50, lambda: self.req(0, fresh=True))
        elif f[1] == 0x84 and self.v3 == "upload" and len(f) >= 9:
            st, off, ln = struct.unpack("<BIH", f[2:9])
            if (off, ln) != self.outstanding:
                return
            if st != 0:
                self.log.append(f"status {st} for {off}+{ln}")
                self.req(self.off, fresh=True)
                return
            data = f[9:]
            if len(data) != ln:
                return
            self.received[off:off + ln] = data
            self.off = off + ln
            self.token += 1
            if self.off >= self.f["size"]:
                self.v3 = "verify"
                self.at(self.verify_ms, self.verdict)
            else:
                self.req(self.off, fresh=True)

    def req(self, off, fresh=False):
        if fresh:
            self.tries = 0
        idx = off // self.chunk
        ln = min(self.chunk, self.f["size"] - off)
        roff = off
        if fresh and self.v3_oob_at == idx and "v3oob" not in self.done_once:
            self.done_once.add("v3oob")
            roff = self.f["size"] + 100
        corrupt = False
        if self.drop_req == idx and "drop_req" not in self.done_once:
            self.done_once.add("drop_req")
            corrupt = True
        self.outstanding = (roff, ln)
        self.bsend(struct.pack("<BBIH", 0x81, 0x04, roff, ln), corrupt=corrupt)
        self.token += 1
        tok = self.token
        self.at(500, lambda: self.req_timeout(tok, off))

    def req_timeout(self, tok, off):
        if tok != self.token or self.mode != "bl" or self.v3 != "upload":
            return
        self.tries += 1
        if self.tries > 5:
            self.v3 = "idle"
            return
        self.req(off)

    def verdict(self):
        ok = bytes(self.received) == self.f["payload"] and not self.bad_image
        self.app_crc = self.f["crc32"] if ok else 0xDEADBEEF
        self.bsend(struct.pack("<BBII", 0x81, 0x82, BTL3, self.app_crc))
        self.v3 = "done"
        if ok:
            self.flashed = True
            self.at(300, lambda: self.boot("app"))


# ---------------------------------------------------------------- driving the harness

def wsl(p: Path) -> str:
    s = str(p).replace("\\", "/")
    return f"/mnt/{s[0].lower()}{s[2:]}"


def build():
    cmd = (f"gcc -std=gnu11 -O1 -g -Wall -Wextra -Werror -fsanitize=address,undefined -fno-sanitize-recover=all "
           f"-I{wsl(MAIN)} {wsl(HERE / 'gcfflash_harness.c')} {wsl(MAIN / 'gcfflash_core.c')} -o {EXE}")
    r = subprocess.run(["wsl", "-d", "Ubuntu-24.04", "--", "bash", "-c", cmd], capture_output=True, text=True)
    if r.returncode:
        print(r.stdout, r.stderr)
        sys.exit("build failed")


def run(args, image: bytes, stick: Stick | None):
    p = subprocess.Popen(["wsl", "-d", "Ubuntu-24.04", "--", EXE, *map(str, args), str(len(image))], stdin=subprocess.PIPE,
                         stdout=subprocess.PIPE, stderr=subprocess.PIPE)
    rd, wr = p.stdout, p.stdin
    logs, prog = [], []

    def need(n):
        b = rd.read(n)
        if len(b) != n:
            err = p.stderr.read().decode(errors="replace")
            raise RuntimeError(f"harness died: {err[-3000:]}")
        return b

    def answer(v: int, extra=b""):
        wr.write(struct.pack("<i", v) + extra)
        wr.flush()

    while True:
        op = need(1)
        if op == b"R":
            mx, to = struct.unpack("<II", need(8))
            d = stick.read(mx, to)
            if d == -1:
                answer(-1)
            else:
                answer(len(d), d)
        elif op == b"W":
            (n,) = struct.unpack("<I", need(4))
            answer(stick.write(need(n)))
        elif op == b"B":
            (b,) = struct.unpack("<I", need(4))
            stick.host_baud = b
            answer(0)
        elif op == b"O":
            (to,) = struct.unpack("<I", need(4))
            answer(stick.reopen(to))
        elif op == b"S":
            (ms,) = struct.unpack("<I", need(4))
            if stick:
                target = stick.now + ms
                stick.run_events(target)
                stick.now = max(stick.now, target)
            answer(0)
        elif op == b"T":
            answer(int(stick.now) if stick else 0)
        elif op == b"I":
            off, n = struct.unpack("<II", need(8))
            if off + n > len(image):
                answer(-1)
            else:
                answer(0, image[off:off + n])
        elif op == b"P":
            (k,) = need(1)
            step = need(k).decode()
            done, total = struct.unpack("<II", need(8))
            prog.append((step, done, total))
            answer(0)
        elif op == b"G":
            (k,) = struct.unpack("<H", need(2))
            logs.append(need(k).decode())
            answer(0)
        elif op == b"X":
            (k,) = struct.unpack("<I", need(4))
            res = json.loads(need(k))
            p.wait(timeout=30)
            err = p.stderr.read().decode(errors="replace")
            if p.returncode != 0 or "runtime error" in err or "AddressSanitizer" in err:
                raise RuntimeError(f"harness exit {p.returncode}: {err[-3000:]}")
            res["logs"], res["progress"] = logs, prog
            return res
        else:
            raise RuntimeError(f"bad op {op!r}")


FAILS = []


def check(name, cond, info=""):
    print(("ok   " if cond else "FAIL ") + name + ("" if cond else f"  -- {info}"))
    if not cond:
        FAILS.append(name)


def gcheck(name, image, fname, dev, *, err_has=None, extra=None):
    res = run(["check", dev, fname], image, None)
    ok = res["check"] == "" if err_has is None else err_has.lower() in res["check"].lower()
    desc = res["check"] or "accepted"
    if extra and ok:
        e = extra(res)
        if e:
            ok, desc = False, f"{desc} | {e}"
    check(f"gcf: {name}", ok, desc)
    return res


def flash(name, image, fname, dev, stick, *, budget=0, expect_ok=True, err_has=None, extra=None):
    res = run(["flash", dev, fname, budget], image, stick)
    desc = f"{res.get('err')} | entered {res['entered']!r} attempts {res['attempts']} reopens {res['reopens']} | {res['logs'][-4:]}"
    ok = bool(res["ok"]) == expect_ok
    if err_has:
        ok &= err_has.lower() in res["err"].lower()
    if expect_ok and ok:
        ok &= bytes(stick.received) == stick.f["payload"] and stick.flashed and stick.mode == "app"
        ok &= res["after"] == stick.new and res["err"] == ""
        if not ok:
            desc += f" | flashed {stick.flashed} mode {stick.mode} after {res['after']:#x}"
    if extra:
        e = extra(res, stick)
        if e:
            ok = False
            desc += f" | {e}"
    check(f"flash: {name}", ok, desc)
    return res


def fetch_real(dest: Path):
    got = []
    for name in (NAME2, NAME3):
        try:
            data = urllib.request.urlopen(FIRMWARE_URL + name, timeout=20).read()
            md5 = urllib.request.urlopen(FIRMWARE_URL + name + ".md5", timeout=20).read().decode().split()[0]
        except Exception as e:  # no network: skip, don't fail
            print(f"skip real {name}: {e}")
            continue
        import hashlib
        if hashlib.md5(data).hexdigest() != md5:
            check(f"real {name} matches its .md5", False, "md5 mismatch")
            continue
        (dest / name).write_bytes(data)
        got.append((name, data))
    return got


def main():
    build()
    v1 = gcf_v1()
    v3 = gcf_v3()
    D2, D3 = 2, 3

    # ---- the file checker
    gcheck("ConBee II file", v1, NAME2, D2, extra=lambda r: None if (r["type"], r["proto"], r["device"], r["platform"], r["target"],
           r["data_size"], r["name_version"]) == (1, 1, 2, 7, 0x5000, 40000, NEW2) and not r["crc_checked"] else r)
    gcheck("ConBee III file (CRC-8 + container CRC-32 checked)", v3, NAME3, D3,
           extra=lambda r: None if (r["type"], r["proto"], r["device"], r["platform"], r["product"], r["crc_checked"])
           == (60, 3, 3, 9, 0xDEC0DE03, 1) and r["crc32"] == parse(v3)["crc32"] else r)
    gcheck("ConBee II file for a ConBee III stick", v1, NAME2, D3, err_has="for a ConBee II")
    gcheck("ConBee III file for a ConBee II stick", v3, NAME3, D2, err_has="for a ConBee III")
    gcheck("name platform 0x07 on a ConBee III container", v3, NAME2, D3, err_has="file name says platform 0x07")
    gcheck("header CRC-8 wrong", gcf_v3(bad_crc8=True), NAME3, D3, err_has="CRC-8")
    flipped = bytearray(v3)
    flipped[5000] ^= 0x10
    gcheck("one flipped bit in the container", bytes(flipped), NAME3, D3, err_has="damaged")
    gcheck("container CRC-32 wrong (CRC-8 fixed up)", gcf_v3(bad_trailer=True), NAME3, D3, err_has="CRC-32")
    gcheck("Hive container", gcf_v3(product=0xDEC0DE02), NAME3, D3, err_has="Hive")
    gcheck("ConBee II file cut short", v1[:-100], NAME2, D2, err_has="cut short")
    gcheck("junk after a ConBee II file", v1 + b"\0" * 10, NAME2, D2, err_has="junk")
    gcheck("not a GCF", b"\x7fELF" + bytes(100), NAME2, D2, err_has="Not a GCF")
    gcheck("too short", b"\xed\xfe\xfe\xca\x01", NAME2, D2, err_has="too short")
    gcheck("bootloader update (type 80)", gcf_v1(ftype=80), NAME2, D2, err_has="bootloader itself")
    gcheck("ConBee I file (type 1 at 0)", gcf_v1(addr=0), "deCONZ_Rev_0x26390500.bin.GCF", D2, err_has="ConBee I")
    gcheck("RaspBee II file", v1, "deCONZ_RaspBeeII_0x26780700.bin.GCF", D2, err_has="RaspBee")
    gcheck("type 90 named for ConBee III", gcf_v90(), NAME3, D3,
           extra=lambda r: None if r["data_offset"] == 18 and r["crc32"] == 0x11223344 and r["data_size"] == 30000 else r)
    gcheck("type 90 without a version in the name", gcf_v90(), "firmware.GCF", D3, err_has="type 90")
    gcheck("no version in the name: header decides", v1, "firmware.gcf", D2, extra=lambda r: None if r["name_version"] == 0 else r)

    reals = []
    files = [(Path(a).name, Path(a).read_bytes()) for a in sys.argv[1:]]
    with tempfile.TemporaryDirectory() as td:
        if not files:
            files = fetch_real(Path(td))
    for name, data in files:
        dev = D3 if "ConBeeIII" in name else D2
        r = gcheck(f"real {name}", data, name, dev, extra=lambda r, d=data, n=name: None if (
            r["data_size"] == len(d) - 14 and (r["crc_checked"] if "ConBeeIII" in n else r["type"] == 1 and r["target"] == 0x5000))
            else r)
        gcheck(f"real {name} refused for the other stick", data, name, D2 if dev == D3 else D3, err_has="the attached stick")
        if r["check"] == "":
            reals.append((name, data, dev))

    # ---- flashing: the happy paths
    def times(r, s):
        return None
    flash("ConBee II: watchdog reset, USB re-enumerates twice, V1 upload", v1, NAME2, D2, Stick(2, v1, NAME2),
          extra=lambda r, s: None if (r["before"], r["entered"], r["reopens"], r["attempts"], r["resent"]) ==
          (OLD2, "watchdog reset (USB re-enumerated)", 2, 1, 0) and "Bootloader" in r["banner"] and s.id_asked >= 1 else r)
    flash("ConBee III: watchdog reset, port stays, V3 upload", v3, NAME3, D3, Stick(3, v3, NAME3),
          extra=lambda r, s: None if (r["before"], r["entered"], r["reopens"], r["bootloader"], r["app_crc_ok"]) ==
          (OLD3, "watchdog reset", 0, BTL3, 1) and r["app_crc"] == s.f["crc32"] else r)
    flash("ConBee III bootloader announces itself: no 6 s wait", v3, NAME3, D3, Stick(3, v3, NAME3, announce=True),
          extra=lambda r, s: None if s.t_update is not None and s.t_update < 3000 else f"update at {s.t_update}")
    flash("ConBee II already in its bootloader", v1, NAME2, D2, Stick(2, v1, NAME2, start_in="bl"),
          extra=lambda r, s: None if r["entered"] == "already in the bootloader" and r["before"] == 0 and r["reopens"] == 1 else r)
    flash("ConBee III already in its bootloader", v3, NAME3, D3, Stick(3, v3, NAME3, start_in="bl"),
          extra=lambda r, s: None if r["entered"] == "already in the bootloader" and r["bootloader"] == BTL3 and s.wdog_count == 0 else r)
    flash("ConBee II firmware chatters (unsolicited frames)", v1, NAME2, D2, Stick(2, v1, NAME2, chatter=True))
    flash("progress reported", v1, NAME2, D2, Stick(2, v1, NAME2),
          extra=lambda r, s: None if r["progress"] and r["progress"][-1] == ("upload", 40000, 40000) else r["progress"][-3:])
    small = gcf_v1(300)
    flash("ConBee II two-page image", small, NAME2, D2, Stick(2, small, NAME2))
    exact = gcf_v1(256 * 20)
    flash("ConBee II image of whole pages", exact, NAME2, D2, Stick(2, exact, NAME2))
    odd = gcf_v3(12345)
    flash("ConBee III odd-sized image, 480-byte chunks", odd, NAME3, D3, Stick(3, odd, NAME3, chunk=480))
    for name, data, dev in reals:
        flash(f"real {name}", data, name, dev, Stick(dev, data, name))

    # ---- refused before touching the stick
    untouched = lambda r, s: None if s.writes == 0 else f"{s.writes} writes"
    flash("ConBee III file on a ConBee II: refused untouched", v3, NAME3, D2, Stick(2, v1, NAME2), expect_ok=False,
          err_has="for a ConBee III", extra=untouched)
    flash("ConBee II file on a ConBee III: refused untouched", v1, NAME2, D3, Stick(3, v3, NAME3), expect_ok=False,
          err_has="for a ConBee II", extra=untouched)
    bad = gcf_v3(bad_crc8=True)
    flash("damaged CRC-8: refused untouched", bad, NAME3, D3, Stick(3, v3, NAME3), expect_ok=False, err_has="CRC-8", extra=untouched)
    bad = gcf_v3(bad_trailer=True)
    flash("damaged CRC-32: refused untouched", bad, NAME3, D3, Stick(3, v3, NAME3), expect_ok=False, err_has="CRC-32", extra=untouched)
    flash("FTDI stick is really a ConBee I (platform 0x05): refused, no watchdog request", v3, NAME3, D3,
          Stick(3, v3, NAME3, app_platform=0x05), expect_ok=False, err_has="platform 0x05",
          extra=lambda r, s: None if s.wdog_count == 0 and r["app_left_running"] and r["before"] == (OLD3 & ~0xFF00) | 0x500 else r)

    # ---- getting into the bootloader goes wrong
    flash("ConBee III firmware ignores the watchdog request", v3, NAME3, D3, Stick(3, v3, NAME3, wdog_works=False),
          expect_ok=False, err_has="never entered its bootloader",
          extra=lambda r, s: None if r["app_left_running"] and r["attempts"] >= 3 and s.now < 75000 and s.received == bytearray(len(s.received)) else r)
    flash("ConBee II resets but comes back in the app", v1, NAME2, D2, Stick(2, v1, NAME2, wdog_works=False, reset_to_app=True),
          expect_ok=False, err_has="never entered", extra=lambda r, s: None if r["app_left_running"] and r["reopens"] >= 2 else r)
    flash("ConBee II never re-enumerates after the reset", v1, NAME2, D2, Stick(2, v1, NAME2, enum_ms=None), expect_ok=False,
          err_has="didn't come back", extra=lambda r, s: None if r["attempts"] == 1 else r)
    flash("ConBee II re-enumerates slowly (8 s)", v1, NAME2, D2, Stick(2, v1, NAME2, enum_ms=8000))
    flash("short budget: gives up in time", v3, NAME3, D3, Stick(3, v3, NAME3, wdog_works=False), budget=10000, expect_ok=False,
          extra=lambda r, s: None if r["attempts"] == 2 and s.now < 25000 else r)

    # ---- the upload goes wrong
    flash("V1: page out of range once: whole sequence retried", v1, NAME2, D2, Stick(2, v1, NAME2, oob_at=60),
          extra=lambda r, s: None if r["attempts"] == 2 and r["entered"] == "already in the bootloader" else r)
    flash("V1: page out of range every time", v1, NAME2, D2, Stick(2, v1, NAME2, oob_at=60, oob_always=True), expect_ok=False,
          err_has="past the end", extra=lambda r, s: None if r["in_bootloader"] and r["attempts"] >= 3 else r)
    flash("V1: 10 bytes of a page lost: the stick asks again", v1, NAME2, D2, Stick(2, v1, NAME2, drop_page=77),
          extra=lambda r, s: None if r["resent"] == 1 and r["attempts"] == 1 else r)
    flash("V1: a GET request garbled: the stick asks again", v1, NAME2, D2, Stick(2, v1, NAME2, lose_get=33),
          extra=lambda r, s: None if r["attempts"] == 1 else r)
    flash("V3: data request out of range: answered with status 1", v3, NAME3, D3, Stick(3, v3, NAME3, v3_oob_at=40),
          extra=lambda r, s: None if r["attempts"] == 1 and any("status 1" in x for x in r["logs"]) else r)
    flash("V3: a data response corrupted: the stick asks again", v3, NAME3, D3, Stick(3, v3, NAME3, corrupt_resp=25),
          extra=lambda r, s: None if r["resent"] == 1 and r["attempts"] == 1 else r)
    flash("V3: a data request garbled: the stick asks again", v3, NAME3, D3, Stick(3, v3, NAME3, drop_req=70),
          extra=lambda r, s: None if r["attempts"] == 1 else r)
    flash("V1: commit never confirmed (60 s)", v1, NAME2, D2, Stick(2, v1, NAME2, never_valid=True), expect_ok=False,
          err_has="#VALID CRC", extra=lambda r, s: None if r["in_bootloader"] and s.now > 60000 else r)
    flash("V1: commit takes 45 s", v1, NAME2, D2, Stick(2, v1, NAME2, commit_ms=45000))
    flash("V1: the reset cuts the commit message off", v1, NAME2, D2, Stick(2, v1, NAME2, reset_before_valid=True),
          extra=lambda r, s: None if any("before its verdict" in x for x in r["logs"]) else r)
    flash("V3: bootloader refuses the update", v3, NAME3, D3, Stick(3, v3, NAME3, refuse=True), expect_ok=False,
          err_has="refused the update (status 1)", extra=lambda r, s: None if r["attempts"] == 1 and r["in_bootloader"] else r)
    flash("V3: image rejected (bad app CRC), stays in the bootloader", v3, NAME3, D3, Stick(3, v3, NAME3, bad_image=True),
          expect_ok=False, err_has="doesn't answer", extra=lambda r, s: None if r["in_bootloader"] and not r["app_crc_ok"] else r)
    flash("new firmware silent", v1, NAME2, D2, Stick(2, v1, NAME2, new_answers=False), expect_ok=False, err_has="doesn't answer")
    flash("new firmware reports another version", v3, NAME3, D3, Stick(3, v3, NAME3, new=0x26490900), expect_ok=False,
          err_has="not the file's", extra=lambda r, s: None if r["app_left_running"] and r["after"] == 0x26490900 else r)

    print(f"\n{len(FAILS)} failed" if FAILS else "\nall passed")
    sys.exit(1 if FAILS else 0)


if __name__ == "__main__":
    main()
