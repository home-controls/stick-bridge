"""Drives the firmware's own dongle flasher (main/ncpflash_core.c, built for the host by ncpflash_harness.c
with ASan + UBSan in WSL) against a simulated Zigbee dongle on a virtual clock:

- EmberZNet firmware: ASH (CRC, randomisation, stuffing, RST/RSTACK, ACKs), EZSP version exchange in the
  legacy and extended formats, launchStandaloneBootloader.
- Gecko bootloader: the menu, "1" + XMODEM-CRC (block numbers, complements, CRC-16, duplicates, NAK, CAN,
  EOT), the GBL CRC-32 check at the end, "2" to run, the RTS/DTR reset pattern.
- Faults: corrupted blocks, lost and garbled ACKs, cancels, rejected images, firmware that won't start or
  won't answer, wrong baud, the USB stick pulled mid-upload, silent firmware, no RTS/DTR circuit.

Plus the GBL checker against good, damaged and dangerous images (and a real Sonoff GBL if one is given).

  python test_ncpflash.py [real.gbl ...]
"""
import json
import random
import struct
import subprocess
import sys
import zlib
from pathlib import Path

HERE = Path(__file__).resolve().parent
MAIN = HERE.parent.parent / "main"
EXE = "/tmp/ncpflash_harness"

FLAG, ESC, XON, XOFF, SUB, CAN_ASH = 0x7E, 0x7D, 0x11, 0x13, 0x18, 0x1A
RESERVED = {FLAG, ESC, XON, XOFF, SUB, CAN_ASH}
SOH, EOT, ACK, NAK, CAN = 0x01, 0x04, 0x06, 0x15, 0x18
MENU = b"\r\nGecko Bootloader v2.4.2\r\n1. upload gbl\r\n2. run\r\n3. ebl info\r\nBL > "

T_HEADER, T_APP, T_META, T_PROG, T_END = 0x03A617EB, 0xF40A0AF4, 0xF60808F6, 0xFE0101FE, 0xFC0404FC
T_BOOTLOADER, T_SE, T_SIG, T_GBL4 = 0xF50909F5, 0x5EA617EB, 0xF70A0AF7, 0x84A617EB
RESIDUE = 0x2144DF1C


# ---------------------------------------------------------------- images

def tag(tid: int, payload: bytes) -> bytes:
    return struct.pack("<II", tid, len(payload)) + payload


def gbl(prog=40000, *, app_type=1, meta='{"metadata_version":1,"fw_type":"zigbee_ncp","baudrate":115200}', extra=b"",
        header_type=0, version=0x03000000, crc_ok=True, trailing=b"", seed=1, end_len=4) -> bytes:
    rnd = random.Random(seed)
    t = tag(T_HEADER, struct.pack("<I", version) + struct.pack("<I", header_type))  # as commander writes it
    t += tag(T_APP, struct.pack("<III", app_type, 0x09010100, 0) + bytes(16))
    if meta:
        t += tag(T_META, meta.encode())
    t += extra
    addr = 0x08006000
    left = prog
    while left > 0:
        n = min(4096, left)
        t += tag(T_PROG, struct.pack("<I", addr) + bytes(rnd.getrandbits(8) for _ in range(n)))
        addr += n
        left -= n
    endhdr = struct.pack("<II", T_END, end_len)
    crc = zlib.crc32(t + endhdr)
    t += endhdr + struct.pack("<I", crc ^ (0 if crc_ok else 1)) + bytes(end_len - 4)
    return t + trailing


def tag_region(img: bytes) -> bytes:
    off = 0
    while off + 8 <= len(img):
        tid, ln = struct.unpack_from("<II", img, off)
        off += 8 + ln
        if tid == T_END:
            return img[:off]
    raise ValueError("no end tag")


def gbl_ok(data: bytes) -> bool:
    try:
        reg = tag_region(data)
    except Exception:
        return False
    return zlib.crc32(reg) == RESIDUE


# ---------------------------------------------------------------- ASH helpers (independent of the C code)

def crc_ash(data: bytes) -> int:
    crc = 0xFFFF
    for b in data:
        crc ^= b << 8
        for _ in range(8):
            crc = ((crc << 1) ^ 0x1021) & 0xFFFF if crc & 0x8000 else (crc << 1) & 0xFFFF
    return crc


def crc_xmodem(data: bytes) -> int:
    crc = 0
    for b in data:
        crc ^= b << 8
        for _ in range(8):
            crc = ((crc << 1) ^ 0x1021) & 0xFFFF if crc & 0x8000 else (crc << 1) & 0xFFFF
    return crc


def lfsr(n: int):
    out, s = [], 0x42
    for _ in range(n):
        out.append(s)
        s = (s >> 1) ^ 0xB8 if s & 1 else s >> 1
    return out


def rnd(data: bytes) -> bytes:
    return bytes(b ^ r for b, r in zip(data, lfsr(len(data))))


def crc_x25(data: bytes) -> int:
    c = 0xFFFF
    for b in data:
        c ^= b
        for _ in range(8):
            c = (c >> 1) ^ 0x8408 if c & 1 else c >> 1
    return ~c & 0xFFFF


def hdlc(payload: bytes) -> bytes:
    raw = payload + struct.pack("<H", crc_x25(payload))
    out = bytearray([0x7E])
    for b in raw:
        if b in (0x7E, 0x7D, 0x11, 0x13, 0xF8):
            out += bytes([0x7D, b ^ 0x20])
        else:
            out.append(b)
    out.append(0x7E)
    return bytes(out)


def ash_frame(control: int, data: bytes = b"") -> bytes:
    body = bytes([control]) + data
    raw = body + struct.pack(">H", crc_ash(body))
    out = bytearray()
    for b in raw:
        out += bytes([ESC, b ^ 0x20]) if b in RESERVED else bytes([b])
    return bytes(out) + bytes([FLAG])


# ---------------------------------------------------------------- the simulated dongle

class Dongle:
    def __init__(self, *, app_baud=115200, ezsp=13, stack=0x7450, app_answers=True, launch_ok=True, rtsdtr=True,
                 start_in="app", app_valid=True, new_ezsp=19, new_stack=0x9110, new_baud=None, new_answers=True,
                 corrupt=None, corrupt_after=None, drop_ack=None, garble=None, cancel_at=None, cancel_text=True, cancel_once=False,
                 early_reject=False, reject=False, no_start=False, gone_at_block=None, seed=0,
                 proto="ezsp", new_proto="ezsp", spinel_ver="SL-OPENTHREAD/2.5.0; EFR32", new_spinel_ver="SL-OPENTHREAD/2.6.1; EFR32",
                 spinel_bl_reset=True):
        self.now = 0.0
        self.host_baud = None
        self.out = []  # (t, byte) in order
        self.events = []  # (t, seq, fn)
        self.evseq = 0
        self.lines = (True, True)
        self.gone = False
        self.rnd = random.Random(seed)
        self.mode = start_in
        self.app_baud, self.ezsp, self.stack = app_baud, ezsp, stack
        self.app_answers, self.launch_ok, self.rtsdtr = app_answers, launch_ok, rtsdtr
        self.app_valid = app_valid if start_in == "app" else app_valid
        self.new = dict(ezsp=new_ezsp, stack=new_stack, baud=new_baud or app_baud, answers=new_answers)
        self.corrupt, self.corrupt_after, self.drop_ack, self.garble = corrupt, corrupt_after, drop_ack, garble
        self.cancel_at, self.cancel_text, self.cancel_once = cancel_at, cancel_text, cancel_once
        self.early_reject, self.reject, self.no_start, self.gone_at_block = early_reject, reject, no_start, gone_at_block
        self.flashed = False
        self.proto, self.new_proto = proto, new_proto
        self.spinel_ver, self.new_spinel_ver, self.spinel_bl_reset = spinel_ver, new_spinel_ver, spinel_bl_reset
        self.sbuf, self.sesc = bytearray(), False
        self.uploads = 0
        self.received = b""
        self.tx_count = 0
        self.log = []
        self.reset_ash()
        self.bl_state = "menu"
        self.xbuf = bytearray()
        if start_in == "bl":
            self.emit(MENU, 115200)

    # time and delivery
    def at(self, dt, fn):
        self.evseq += 1
        self.events.append((self.now + dt, self.evseq, fn))
        self.events.sort()

    def emit(self, data: bytes, baud: int, delay=1.0):
        t = max(self.now + delay, self.out[-1][0] if self.out else 0)
        for b in data:
            t += 10000.0 / baud  # ms per byte
            self.out.append((t, b, baud))

    def run_events(self, upto):
        while self.events and self.events[0][0] <= upto:
            t, _, fn = self.events.pop(0)
            self.now = max(self.now, t)
            fn()

    def read(self, mx, timeout):
        if self.gone:
            self.now += 1
            return -1
        deadline = self.now + timeout
        while True:
            self.run_events(self.now)
            got = bytearray()
            while self.out and self.out[0][0] <= self.now and len(got) < mx:
                _, b, baud = self.out.pop(0)
                if baud == self.host_baud:  # sent at another baud: garbage the host never sees as data
                    got.append(b)
            if got:
                return bytes(got)
            if self.now >= deadline:
                return b""
            t = min([deadline] + ([self.out[0][0]] if self.out else []) + ([self.events[0][0]] if self.events else []))
            self.now = max(self.now, t)

    def write(self, data: bytes):
        if self.gone:
            return -1
        self.now += len(data) * 10000.0 / (self.host_baud or 115200)
        self.run_events(self.now)
        if self.mode == "app":
            if self.host_baud == self.app_baud and self.app_answers:
                for b in data:
                    if self.proto == "spinel":
                        self.spinel_byte(b)
                    else:
                        self.ash_byte(b)
        elif self.mode == "bl":
            if self.host_baud == 115200:
                self.bl_bytes(data)
        return 0

    def set_lines(self, dtr, rts):
        prev, self.lines = self.lines, (dtr, rts)
        if prev == (False, True) and self.lines == (True, False) and self.rtsdtr:
            self.log.append("rtsdtr reset")
            self.enter_bl()
        return 0

    # EmberZNet side
    def reset_ash(self):
        self.abuf, self.aesc, self.ncp_frm, self.ext = bytearray(), False, 0, False

    def ash_byte(self, b):
        if b == FLAG:
            raw, self.abuf, self.aesc = bytes(self.abuf), bytearray(), False
            if len(raw) >= 3 and crc_ash(raw[:-2]) == struct.unpack(">H", raw[-2:])[0]:
                self.ash_frame(raw[:-2])
        elif b == CAN_ASH:
            self.abuf, self.aesc = bytearray(), False
        elif b == ESC:
            self.aesc = True
        elif b in (XON, XOFF, SUB):
            pass
        else:
            if self.aesc:
                b ^= 0x20
                self.aesc = False
            self.abuf.append(b)

    def ash_frame(self, f: bytes):
        c = f[0]
        if c == 0xC0:
            self.reset_ash()
            self.emit(ash_frame(0xC1, bytes([2, 0x0B])), self.app_baud, delay=30)
            return
        if c & 0x80:
            return
        frm = (c >> 4) & 7
        d = rnd(f[1:])
        seq = d[0]
        if len(d) >= 5 and (d[2] & 3) == 1:
            fid, params, ext = d[3] | (d[4] << 8), d[5:], True
        else:
            fid, params, ext = d[2], d[3:], False
        if fid == 0x0000:
            body = bytes([self.ezsp, 2]) + struct.pack("<H", self.stack)
            if params and params[0] == self.ezsp:
                self.ext = self.ezsp >= 8
        elif fid == 0x008F and ext == self.ext:
            st = 0 if self.launch_ok else 1
            body = struct.pack("<I", st) if self.ezsp >= 14 else bytes([st])
            if self.launch_ok:
                self.at(200, self.enter_bl)
        else:
            return
        hdr = bytes([seq, 0x80, 0x01]) + struct.pack("<H", fid) if ext else bytes([seq, 0x80, fid])
        ctl = (self.ncp_frm << 4) | ((frm + 1) & 7)
        self.ncp_frm = (self.ncp_frm + 1) & 7
        self.emit(ash_frame(ctl, rnd(hdr + body)), self.app_baud, delay=5)

    # OpenThread RCP side (Spinel over HDLC-lite)
    def spinel_byte(self, b):
        if b == 0x7E:
            raw, self.sbuf, self.sesc = bytes(self.sbuf), bytearray(), False
            if len(raw) >= 3 and crc_x25(raw[:-2]) == struct.unpack("<H", raw[-2:])[0]:
                self.spinel_frame(raw[:-2])
        elif b == 0x7D:
            self.sesc = True
        else:
            self.sbuf.append(b ^ 0x20 if self.sesc else b)
            self.sesc = False

    def spinel_frame(self, f: bytes):
        if len(f) >= 3 and f[1] == 0x02 and f[2] == 0x02:  # PROP_VALUE_GET NCP_VERSION
            self.emit(hdlc(bytes([f[0], 0x06, 0x02]) + self.spinel_ver.encode() + b"\0"), self.app_baud, delay=5)
        elif len(f) >= 3 and f[1] == 0x01 and f[2] == 0x03:  # CMD_RESET bootloader
            self.log.append("spinel bootloader reset")
            if self.spinel_bl_reset:
                self.at(300, self.enter_bl)

    # bootloader side
    def enter_bl(self):
        self.mode, self.bl_state, self.xbuf = "bl", "menu", bytearray()
        self.emit(MENU, 115200, delay=50)

    def start_app(self):
        self.mode = "app"
        self.reset_ash()
        self.sbuf, self.sesc = bytearray(), False
        if self.flashed:
            self.ezsp, self.stack, self.app_baud, self.app_answers = (self.new["ezsp"], self.new["stack"], self.new["baud"],
                                                                      self.new["answers"])
            self.proto, self.spinel_ver = self.new_proto, self.new_spinel_ver

    def send_c(self):
        if self.mode == "bl" and self.bl_state == "wait_c":
            self.emit(b"C", 115200)
            self.at(1000, self.send_c)

    def bl_bytes(self, data: bytes):
        for b in data:
            if self.bl_state == "menu":
                if b == ord("3"):
                    self.emit(MENU, 115200)
                elif b == ord("1"):
                    self.uploads += 1
                    self.emit(b"\r\nbegin upload\r\n", 115200)
                    self.bl_state, self.expect, self.received, self.blocks = "wait_c", 1, b"", 0
                    self.at(10, self.send_c)
                elif b == ord("2"):
                    if self.app_valid and not self.no_start:
                        self.at(100, self.start_app)
                    else:
                        self.emit(MENU, 115200, delay=300)
            else:
                self.xbuf.append(b)
                self.xmodem()

    def xmodem(self):
        while self.xbuf:
            if self.xbuf[0] == EOT:
                del self.xbuf[0]
                self.emit(bytes([ACK]), 115200)
                self.bl_state = "menu"
                if self.reject:
                    self.emit(b"\r\nSerial upload aborted\r\n\r\nerror 0x0d (signature)\r\n" + MENU, 115200, delay=40)
                elif gbl_ok(self.received):
                    self.app_valid, self.flashed = True, True
                    self.emit(b"\r\nSerial upload complete\r\n" + MENU, 115200, delay=40)
                else:
                    self.emit(b"\r\nSerial upload aborted\r\n\r\nerror 0x03 (crc)\r\n" + MENU, 115200, delay=40)
                return
            if self.xbuf[0] == CAN and self.bl_state == "xmodem":  # the sender gave up
                self.xbuf.clear()
                self.bl_state = "menu"
                self.emit(b"\r\nSerial upload aborted\r\n\r\nerror 0x29 (cancelled)\r\n" + MENU, 115200, delay=20)
                return
            if self.xbuf[0] != SOH:
                del self.xbuf[0]
                continue
            if len(self.xbuf) < 133:
                return
            pkt, self.xbuf = bytes(self.xbuf[:133]), self.xbuf[133:]
            self.bl_state = "xmodem"
            self.tx_count += 1
            if (self.corrupt and self.tx_count % self.corrupt == 0) or (
                    self.corrupt_after is not None and self.blocks >= self.corrupt_after):
                pkt = pkt[:50] + bytes([pkt[50] ^ 0x40]) + pkt[51:]
            num, inv, payload, crc = pkt[1], pkt[2], pkt[3:131], struct.unpack(">H", pkt[131:133])[0]
            if num != 255 - inv or crc_xmodem(payload) != crc:
                self.emit(bytes([NAK]), 115200)
                continue
            if num == (self.expect - 1) & 0xFF and self.blocks:
                self.emit(bytes([ACK]), 115200)  # duplicate: the sender missed our ACK
                continue
            if num != self.expect & 0xFF:
                self.emit(bytes([NAK]), 115200)
                continue
            if self.early_reject:
                self.bl_state = "menu"
                self.emit(bytes([CAN, CAN, CAN]) + b"\r\nSerial upload aborted\r\n\r\nerror 0x0e (wrong product)\r\n" + MENU, 115200)
                return
            if self.cancel_at is not None and self.blocks == self.cancel_at and not (self.cancel_once and self.uploads > 1):
                self.bl_state = "menu"
                self.app_valid = False
                txt = b"\r\nSerial upload aborted\r\n\r\nerror 0x22\r\n" + MENU if self.cancel_text else b""
                self.emit(bytes([CAN, CAN, CAN]) + txt, 115200)
                return
            self.app_valid = False  # the bootloader writes over the old firmware as blocks arrive
            self.received += payload
            self.blocks += 1
            self.expect += 1
            if self.gone_at_block is not None and self.blocks >= self.gone_at_block:
                self.gone = True
                return
            if self.drop_ack and self.blocks % self.drop_ack == 0:
                continue
            if self.garble and self.blocks % self.garble == 0:
                self.emit(b"\x41", 115200)
                continue
            self.emit(bytes([ACK]), 115200)


# ---------------------------------------------------------------- driving the harness

def wsl(p: Path) -> str:
    s = str(p).replace("\\", "/")
    return f"/mnt/{s[0].lower()}{s[2:]}"


def build():
    cmd = (f"gcc -std=gnu11 -O1 -g -Wall -Wextra -Werror -fsanitize=address,undefined -fno-sanitize-recover=all "
           f"-I{wsl(MAIN)} {wsl(HERE / 'ncpflash_harness.c')} {wsl(MAIN / 'ncpflash_core.c')} -o {EXE}")
    r = subprocess.run(["wsl", "-d", "Ubuntu-24.04", "--", "bash", "-c", cmd], capture_output=True, text=True)
    if r.returncode:
        print(r.stdout, r.stderr)
        sys.exit("build failed")


def run(args, image: bytes, dongle: Dongle | None, spinel=False):
    env = ["env", "NF_EXPECT_SPINEL=1"] if spinel else []
    p = subprocess.Popen(["wsl", "-d", "Ubuntu-24.04", "--", *env, EXE, *map(str, args), str(len(image))], stdin=subprocess.PIPE,
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
            d = dongle.read(mx, to)
            if d == -1:
                answer(-1)
            else:
                answer(len(d), d)
        elif op == b"W":
            (n,) = struct.unpack("<I", need(4))
            answer(dongle.write(need(n)))
        elif op == b"B":
            (b,) = struct.unpack("<I", need(4))
            dongle.host_baud = b
            answer(-1 if dongle.gone else 0)
        elif op == b"L":
            d, r = need(2)
            answer(dongle.set_lines(bool(d), bool(r)))
        elif op == b"S":
            (ms,) = struct.unpack("<I", need(4))
            if dongle:
                target = dongle.now + ms
                dongle.run_events(target)
                dongle.now = max(dongle.now, target)
            answer(0)
        elif op == b"T":
            answer(int(dongle.now) if dongle else 0)
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


def flash(name, image, dongle, *, app_baud=115200, new_baud=115200, expect_ok=True, err_has=None, extra=None, spinel=False):
    res = run(["flash", app_baud, new_baud], image, dongle, spinel=spinel)
    desc = f"{res.get('err') or res.get('check')} | {res['logs'][-3:]}"
    ok = bool(res.get("ok")) == expect_ok
    if err_has:
        ok &= err_has.lower() in (res.get("err") or res.get("check") or "").lower()
    if expect_ok and ok:
        want = tag_region(image)
        pad = (-len(want)) % 128
        ok &= dongle.received == want + b"\xff" * pad
        ok &= dongle.mode == "app" and dongle.flashed
        if not ok:
            desc += f" | received {len(dongle.received)} vs {len(want) + pad}, mode {dongle.mode}"
    if extra:
        e = extra(res, dongle)
        if e:
            ok = False
            desc += f" | {e}"
    check(f"flash: {name}", ok, desc)
    return res


def gcheck(name, image, *, err_has=None, extra=None):
    res = run(["check"], image, None)
    if err_has is None:
        ok = res["check"] == ""
    else:
        ok = err_has.lower() in res["check"].lower()
    desc = res["check"]
    if extra and ok:
        e = extra(res)
        if e:
            ok = False
            desc += f" | {e}"
    check(f"gbl: {name}", ok, desc)
    return res


def main():
    build()
    img = gbl()
    tr = tag_region(img)

    # ---- the GBL checker
    gcheck("good image", img, extra=lambda r: None if r["tag_len"] == len(img) and r["prog"] == 40000 and "zigbee_ncp" in r["meta"]
           else r)
    gcheck("trailing junk is ignored, not sent", img + b"\xff" * 300 + b"junk",
           extra=lambda r: None if r["tag_len"] == len(img) and r["trailing"] == 304 else r)
    gcheck("CRC-32 wrong", gbl(crc_ok=False), err_has="CRC-32")
    gcheck("one flipped bit in the firmware", img[:5000] + bytes([img[5000] ^ 1]) + img[5001:], err_has="CRC-32")
    gcheck("cut short", img[:-100], err_has="cut short")
    gcheck("no end tag", tr[:-12], err_has="cut short")
    gcheck("GBL v4", struct.pack("<II", T_GBL4, 8) + bytes(100), err_has="v4")
    gcheck("not a GBL (ESP32 image)", bytes([0xE9, 3, 2, 0x20]) + bytes(2000), err_has="Not a GBL")
    gcheck("empty", b"", err_has="too short")
    gcheck("5 bytes", b"\xeb\x17\xa6\x03\x08", err_has="too short")
    gcheck("bootloader upgrade inside", gbl(extra=tag(T_BOOTLOADER, bytes(64))), err_has="bootloader")
    gcheck("secure element upgrade inside", gbl(extra=tag(T_SE, bytes(64))), err_has="secure element")
    gcheck("Thread firmware", gbl(app_type=2), err_has="Thread")
    gcheck("Bluetooth firmware", gbl(app_type=8), err_has="Bluetooth")
    gcheck("unknown tag", gbl(extra=tag(0x12345678, b"x")), err_has="unknown tag")
    gcheck("no firmware inside", gbl(prog=0), err_has="no firmware")
    gcheck("second header", gbl(extra=tag(T_HEADER, bytes(8))), err_has="second GBL header")
    gcheck("end tag of the wrong size", gbl(end_len=5), err_has="end tag")
    huge = bytearray(img)
    struct.pack_into("<I", huge, 4 + 8 + 4 + 4, 0xFFFFFFF0)  # second tag's length
    gcheck("tag length past the end (overflow)", bytes(huge), err_has="cut short")
    gcheck("GBL v2 header", gbl(version=0x02000000), err_has="v3")
    gcheck("long metadata is cut, not overflowed", gbl(meta="x" * 3000),
           extra=lambda r: None if r["meta_cut"] and len(r["meta"]) == 511 else r)
    gcheck("signed + encrypted flags read", gbl(header_type=0x101, extra=tag(T_SIG, bytes(64))),
           extra=lambda r: None if r["signed"] and r["encrypted"] else r)
    gcheck("no metadata", gbl(meta=""), extra=lambda r: None if r["meta"] == "" else r)
    reals = []
    for real in sys.argv[1:]:
        data = Path(real).read_bytes()
        res = run(["check"], data, None)
        print(f"     real file {Path(real).name}: {res['check'] or 'accepted'} (meta {res['meta'][:80]})")
        if res["check"] == "":
            reals.append((Path(real).name, data))

    # ---- flashing
    asked = lambda r, d: None if r["entered"] == "asked the firmware" else r["entered"]
    flash("happy path, EZSP 13 -> 19 (>255 blocks, block numbers wrap)", img, Dongle(),
          extra=lambda r, d: None if r["before"] == [1, 13, 0x7450] and r["after"] == [1, 19, 0x9110] and
          r["entered"] == "asked the firmware" and r["retries"] == 0 else r)
    flash("old firmware already EZSP 19", img, Dongle(ezsp=19, stack=0x9110, new_ezsp=19, new_stack=0x9120), extra=asked)
    flash("firmware at 460800, bootloader at 115200", img, Dongle(app_baud=460800), app_baud=460800, new_baud=460800,
          extra=asked)
    flash("dongle already in its bootloader (no firmware)", img, Dongle(start_in="bl", app_valid=False),
          extra=lambda r, d: None if r["entered"] == "already in the bootloader" else r["entered"])
    flash("firmware hung: RTS/DTR reset", img, Dongle(app_answers=False),
          extra=lambda r, d: None if r["entered"] == "RTS/DTR reset" else r["entered"])
    flash("firmware hung, no RTS/DTR circuit", img, Dongle(app_answers=False, rtsdtr=False), expect_ok=False,
          err_has="couldn't get the dongle into its bootloader",
          extra=lambda r, d: None if d.mode == "app" and d.received == b"" else "touched the dongle")
    flash("firmware refuses to start the bootloader, no RTS/DTR", img, Dongle(launch_ok=False, rtsdtr=False), expect_ok=False,
          err_has="bootloader", extra=lambda r, d: None if r["app_left_running"] and d.mode == "app" else r)
    flash("firmware refuses, RTS/DTR works", img, Dongle(launch_ok=False),
          extra=lambda r, d: None if r["entered"] == "RTS/DTR reset" else r["entered"])
    flash("every 7th block corrupted on the wire", img, Dongle(corrupt=7),
          extra=lambda r, d: None if r["retries"] > 40 else r["retries"])
    flash("every 50th ACK lost (duplicates)", img, Dongle(drop_ack=50),
          extra=lambda r, d: None if r["retries"] >= 6 else r["retries"])
    flash("every 30th ACK garbled", img, Dongle(garble=30))
    flash("bootloader cancels once, no reason: retried", img, Dongle(cancel_at=100, cancel_text=False, cancel_once=True),
          extra=lambda r, d: None if d.uploads == 2 else f"uploads {d.uploads}")
    flash("bootloader rejects mid-way with a reason: not retried", img, Dongle(cancel_at=100), expect_ok=False,
          err_has="rejected the image at",
          extra=lambda r, d: None if d.uploads == 1 and r["in_bootloader"] and "error 0x22" in r["err"] else r)
    flash("bootloader rejects at the end (signature)", img, Dongle(reject=True), expect_ok=False, err_has="error 0x0d",
          extra=lambda r, d: None if r["in_bootloader"] and d.uploads == 1 else r)
    flash("bootloader rejects at the first block: old firmware restarted", img, Dongle(early_reject=True), expect_ok=False,
          err_has="wrong product",
          extra=lambda r, d: None if r["app_left_running"] and d.mode == "app" and r["after"][1] == 13 else r)
    flash("every block corrupted: gives up, old firmware restarted", img, Dongle(corrupt=1), expect_ok=False,
          err_has="kept failing",
          extra=lambda r, d: None if r["app_left_running"] and d.mode == "app" and d.uploads == 2 else r)
    flash("line dies at 40%: gives up, left in the bootloader", img, Dongle(corrupt_after=125), expect_ok=False,
          err_has="kept failing at 39%",
          extra=lambda r, d: None if r["in_bootloader"] and d.mode == "bl" and d.uploads == 2 else r)
    flash("new firmware won't start", img, Dongle(no_start=True), expect_ok=False, err_has="didn't start",
          extra=lambda r, d: None if r["in_bootloader"] else r)
    flash("new firmware silent", img, Dongle(new_answers=False), expect_ok=False, err_has="doesn't answer")
    flash("new firmware at another baud than expected", img, Dongle(new_baud=460800), expect_ok=False,
          err_has="at 115200 baud")
    flash("new firmware at the baud it says", img, Dongle(new_baud=460800), new_baud=460800)
    flash("USB stick pulled mid-upload", img, Dongle(gone_at_block=150), expect_ok=False, err_has="went away")
    flash("trailing junk not sent", img + b"\x00" * 1000, Dongle())
    small = gbl(prog=60, meta="")
    flash(f"tiny image ({len(small)} bytes)", small, Dongle())
    exact = next(g for g in (gbl(prog=p) for p in range(39000, 39200)) if len(g) % 128 == 0)
    flash(f"image a multiple of 128 bytes ({len(exact)}, {len(exact) % 128})", exact, Dongle())
    flash("progress reported", img, Dongle(),
          extra=lambda r, d: None if any(p[0] == "upload" and p[1] == len(tr) == p[2] for p in r["progress"]) else r["progress"][-3:])
    for name, data in reals:
        flash(f"real file {name}", data, Dongle())
    # OpenThread RCP builds (Thread role): Spinel verification and the Spinel way into the bootloader
    rcp = gbl(meta='{"metadata_version":3,"fw_type":"openthread_rcp","baudrate":460800}')
    sp_ok = lambda want: (lambda r, d: None if r["after_spinel"].startswith(want) else f"after_spinel={r['after_spinel']!r}")
    flash("zigbee -> thread: verified over Spinel", rcp, Dongle(new_proto="spinel", new_baud=460800), new_baud=460800, spinel=True,
          extra=lambda r, d: sp_ok("SL-OPENTHREAD/2.6.1")(r, d) or (None if r["entered"] == "asked the firmware" else r["entered"]))
    flash("thread -> zigbee: Spinel bootloader reset", img, Dongle(proto="spinel", app_baud=460800, new_proto="ezsp", new_baud=115200),
          app_baud=460800, new_baud=115200,
          extra=lambda r, d: (None if r["entered"] == "asked the firmware (Spinel)" else r["entered"])
          or (None if r["before_spinel"].startswith("SL-OPENTHREAD/2.5.0") else r["before_spinel"]))
    flash("thread -> thread at another baud", rcp, Dongle(proto="spinel", app_baud=115200, new_proto="spinel", new_baud=460800),
          app_baud=115200, new_baud=460800, spinel=True, extra=sp_ok("SL-OPENTHREAD/2.6.1"))
    flash("rcp without bootloader reset: DTR/RTS fallback", rcp,
          Dongle(proto="spinel", app_baud=460800, spinel_bl_reset=False, new_proto="spinel", new_baud=460800),
          app_baud=460800, new_baud=460800, spinel=True,
          extra=lambda r, d: None if r["entered"] == "RTS/DTR reset" else r["entered"])
    flash("expects thread but the firmware is a coordinator", rcp, Dongle(new_proto="ezsp"), spinel=True, expect_ok=False,
          err_has="OpenThread RCP")
    flash("damaged image never reaches the dongle", gbl(crc_ok=False), Dongle(), expect_ok=False, err_has="CRC-32",
          extra=lambda r, d: None if d.uploads == 0 and d.mode == "app" else "dongle touched")

    print(f"\n{len(FAILS)} failed" if FAILS else "\nall passed")
    sys.exit(1 if FAILS else 0)


if __name__ == "__main__":
    main()
