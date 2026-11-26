"""Drives the firmware's own nRF52840 Dongle flasher (main/nrfdfu_core.c, built for the host by nrfdfu_harness.c
with ASan + UBSan in WSL) against a simulated PCA10059 Open USB bootloader on a virtual clock.

The simulator follows the nRF5 SDK 17.1 bootloader sources, independently of the C code:
- serial DFU over USB CDC (nrf_dfu_serial.c, nrf_dfu_serial_usb.c, slip.c): SLIP framing, MTU 2051, PRN,
  replies dropped until the host sets DTR (app_usbd_cdc_acm_write), an empty packet flagged as a violation;
- the request handler (nrf_dfu_req_handler.c): select/create/write/CRC/execute for command and data objects,
  page-aligned 4 KB data objects, erase time, the final execute's SHA-256 postvalidation and reset;
- init packet decoding like nanopb (dfu-cc.proto + dfu-cc.options) and the checks of nrf_dfu_validation.c /
  nrf_dfu_ver_validation.c with the open bootloader's config (hw_version 52, unsigned apps allowed, no
  downgrade prevention), single-bank: the old app is dropped when the init packet is accepted;
- the firmware side: an app with or without the nRF5 SDK USB DFU trigger.
Faults: dropped and bit-flipped writes, lost replies, stray frames, SLIP garbage, the stick pulled, a flash
error caught by the hash, secure (signed-only) bootloaders, wrong hw_version, SoftDevice requirements, tiny
MTU, small init buffers, no room.

Init packets are checked against golden bytes made with Google protobuf from pc-nrfutil's dfu-cc.proto the
way pc-nrfutil's InitPacketPB builds them. Intel HEX conversion is checked against an nrfutil-style
reference (and real .hex files given on the command line).

  python test_nrfdfu.py [real.hex ...]
"""
import hashlib
import json
import random
import struct
import subprocess
import sys
import zlib
from pathlib import Path

HERE = Path(__file__).resolve().parent
MAIN = HERE.parent.parent / "main"
EXE = "/tmp/nrfdfu_harness"

END, ESC, ESC_END, ESC_ESC = 0xC0, 0xDB, 0xDC, 0xDD
OP_CREATE, OP_PRN, OP_CRC, OP_EXECUTE, OP_SELECT, OP_MTU, OP_WRITE, OP_PING, OP_RSP = 1, 2, 3, 4, 6, 7, 8, 9, 0x60
SUCCESS, NOT_SUPPORTED, INVALID_PARAM, NO_RESOURCES, INVALID_OBJECT, NOT_PERMITTED, EXT = 1, 2, 3, 4, 5, 8, 0x0B
EXT_INIT_INVALID, EXT_HW, EXT_SD, EXT_SIG_MISSING, EXT_HASH_TYPE, EXT_VERIFY = 4, 6, 7, 8, 9, 0x0C

# golden init packets: Google protobuf + pc-nrfutil's InitPacketPB logic (application, sd_req 0x00)
GOLDEN_A = ("0a420801123e080110341a0100200028003000388827422408031220a1c8dc3ed8d9d9e05cfaeade7e062fcd0068c8a89b4c021353"
            "ba5772f25bdab24800520408011200")  # random.Random(7) 5000 bytes, fw 1, hw 52
GOLDEN_B = ("0a470801124308f8acd1910110341a01002000280030003884d00f42240803122077799b27d51095c161120ce3a56f1b6096126bf1"
            "d77b74f92d52920b334dd0cd4800520408011200")  # bytes(range(256))*1000 + 01020304, fw 0x12345678, hw 52


# ---------------------------------------------------------------- protobuf (independent of the C code)

def varint(v: int) -> bytes:
    out = bytearray()
    while True:
        b = v & 0x7F
        v >>= 7
        out.append(b | (0x80 if v else 0))
        if not v:
            return bytes(out)


def fld(n: int, wt: int, payload) -> bytes:
    key = varint(n << 3 | wt)
    if wt == 0:
        return key + varint(payload)
    return key + varint(len(payload)) + payload


def make_init(app: bytes, *, fw=1, hw=52, sd_req=(0,), fw_type=0, hash_type=3, signed=False, both=False, app_size=None,
              packed=True, drop=(), digest=None) -> bytes:
    h = (digest or hashlib.sha256(app).digest())[::-1]
    f = {1: fld(1, 0, fw), 2: fld(2, 0, hw),
         3: fld(3, 2, b"".join(varint(x) for x in sd_req)) if packed else b"".join(fld(3, 0, x) for x in sd_req),
         4: fld(4, 0, fw_type), 5: fld(5, 0, 0), 6: fld(6, 0, 0), 7: fld(7, 0, len(app) if app_size is None else app_size),
         8: fld(8, 2, fld(1, 0, hash_type) + fld(2, 2, h)), 9: fld(9, 0, 0), 10: fld(10, 2, fld(1, 0, 1) + fld(2, 2, b""))}
    init = b"".join(v for k, v in f.items() if k not in drop)
    cmd = fld(1, 0, 1) + fld(2, 2, init)
    if signed:
        sc = fld(1, 2, cmd) + fld(2, 0, 0) + fld(3, 2, bytes(64))
        return fld(2, 2, sc) + (fld(1, 2, cmd) if both else b"")
    return fld(1, 2, cmd)


class PBError(Exception):
    pass


def pb_parse(data: bytes):
    """field -> list of values (int, or bytes for length-delimited)."""
    out, i = {}, 0

    def rv():
        nonlocal i
        v = s = 0
        while True:
            if i >= len(data):
                raise PBError("cut")
            b = data[i]
            i += 1
            v |= (b & 0x7F) << s
            s += 7
            if not b & 0x80:
                return v
            if s > 63:
                raise PBError("varint")
    while i < len(data):
        key = rv()
        n, wt = key >> 3, key & 7
        if wt == 0:
            val = rv()
        elif wt == 2:
            ln = rv()
            if i + ln > len(data):
                raise PBError("cut")
            val = data[i:i + ln]
            i += ln
        elif wt in (1, 5):
            k = 8 if wt == 1 else 4
            if i + k > len(data):
                raise PBError("cut")
            val = data[i:i + k]
            i += k
        else:
            raise PBError("wire type")
        out.setdefault(n, []).append((wt, val))
    return out


def decode_init(dat: bytes):
    """Like stored_init_cmd_decode() + nanopb with dfu-cc.options: dict of the InitCommand, or None."""
    try:
        pkt = pb_parse(dat)
        signed = 2 in pkt
        if signed and 1 in pkt:
            return None
        if signed:
            sc = pb_parse(pkt[2][-1][1])
            if not all(k in sc for k in (1, 2, 3)) or len(sc[3][-1][1]) > 64:
                return None
            cmd = pb_parse(sc[1][-1][1])
        elif 1 in pkt:
            cmd = pb_parse(pkt[1][-1][1])
        else:
            return None
        if 2 not in cmd:
            return None
        raw = pb_parse(cmd[2][-1][1])
        ic = {"signed": signed}
        for n, name in ((1, "fw_version"), (2, "hw_version"), (4, "type"), (7, "app_size"), (9, "is_debug")):
            if n in raw:
                wt, v = raw[n][-1]
                if wt != 0 or v > 0xFFFFFFFF:
                    return None
                ic[name] = v
        sd = []
        for wt, v in raw.get(3, []):
            if wt == 2:
                j = 0
                while j < len(v):
                    x = s = 0
                    while True:
                        b = v[j]
                        j += 1
                        x |= (b & 0x7F) << s
                        s += 7
                        if not b & 0x80:
                            break
                    sd.append(x)
            else:
                sd.append(v)
        if len(sd) > 16:
            return None
        ic["sd_req"] = sd
        if 8 in raw:
            hh = pb_parse(raw[8][-1][1])
            if 1 not in hh or 2 not in hh or len(hh[2][-1][1]) > 32:
                return None
            ic["hash_type"], ic["hash"] = hh[1][-1][1], hh[2][-1][1]
        return ic
    except (PBError, IndexError):
        return None


# ---------------------------------------------------------------- images

def app_image(n: int, *, seed=1, sp=0x20017F80, reset=None, base=0x1000) -> bytes:
    rnd = random.Random(seed)
    body = bytes(rnd.getrandbits(8) for _ in range(max(0, n - 8)))
    rv = (base + 0x200) | 1 if reset is None else reset
    return (struct.pack("<II", sp, rv) + body)[:n]


def ihex(chunks, *, uicr=b"", eof=True, seg=False, shuffle=False, bad_sum=False, width=16) -> str:
    """chunks: [(addr, bytes)] -> Intel HEX text, with type-04 (or type-02) address records."""
    recs = []
    for addr, data in chunks:
        for o in range(0, len(data), width):
            a = addr + o
            part = data[o:o + width]
            recs.append((a, part))
    if uicr:
        recs.append((0x10001000, uicr))
    if shuffle:
        random.Random(3).shuffle(recs)
    lines, upper = [], None

    def rec(t, a, d):
        b = bytes([len(d), (a >> 8) & 0xFF, a & 0xFF, t]) + d
        s = (-sum(b)) & 0xFF
        if bad_sum:
            s ^= 1
        return ":" + (b + bytes([s])).hex().upper()
    for a, d in recs:
        if seg:
            hi = a >> 4 & 0xF000
            if hi != upper:
                lines.append(rec(2, 0, struct.pack(">H", hi)))
                upper = hi
            lines.append(rec(0, a - (hi << 4), d))
        else:
            hi = a >> 16
            if hi != upper:
                lines.append(rec(4, 0, struct.pack(">H", hi)))
                upper = hi
            lines.append(rec(0, a & 0xFFFF, d))
    if eof:
        lines.append(":00000001FF")
    return "\r\n".join(lines) + "\r\n"


def hex_reference(text: str) -> bytes:
    """What nrfutil's nRFHex.tobinfile makes of an application .hex."""
    buf, upper = {}, 0
    for line in text.split():
        b = bytes.fromhex(line[1:])
        n, a, t = b[0], b[1] << 8 | b[2], b[3]
        if t == 0:
            for i in range(n):
                buf[upper + a + i] = b[4 + i]
        elif t == 2:
            upper = (b[4] << 8 | b[5]) << 4
        elif t == 4:
            upper = (b[4] << 8 | b[5]) << 16
        elif t == 1:
            break
    buf = {k: v for k, v in buf.items() if 0x1000 <= k < 0x10000000}
    lo, hi = min(buf), max(buf)
    size = (hi - lo + 1 + 3) // 4 * 4
    return bytes(buf.get(lo + i, 0xFF) for i in range(size))


# ---------------------------------------------------------------- the simulated dongle

class Dongle:
    MTU_USB = 2 * (1024 + 1) + 1
    PAGE = 4096

    def __init__(self, *, mode="bl", trigger=False, trigger_works=True, hw=52, require_signed=False, sd_present=False,
                 sd_fwid=0xCA, mtu=MTU_USB, init_max=512, app_max=0xE0000 - 0x3000 - 0x1000, drop_writes=(),
                 flip_writes=(), flip_all_in_object=None, lose_reply=None, stray_before=None, garbage_before=None,
                 gone_at_object=None, flash_error=False, erase_ms=85):
        self.now = 0.0
        self.out = []  # (t, bytes)
        self.mode, self.trigger, self.trigger_works = mode, trigger, trigger_works
        self.hw, self.require_signed, self.sd_present, self.sd_fwid = hw, require_signed, sd_present, sd_fwid
        self.mtu, self.init_max, self.app_max = mtu, init_max, app_max
        self.drop_writes, self.flip_writes, self.flip_all_in_object = set(drop_writes), set(flip_writes), flip_all_in_object
        self.lose_reply, self.stray_before, self.garbage_before = lose_reply, stray_before, garbage_before
        self.gone_at_object, self.flash_error, self.erase_ms = gone_at_object, flash_error, erase_ms
        self.dtr = False
        self.gone = False
        self.violations = []
        self.slip, self.esc, self.junk = bytearray(), False, False
        # bootloader state (s_dfu_settings.progress + req handler)
        self.prn = self.prn_count = 0
        self.cur = 1  # nrf_dfu_obj_op's current_object: write/CRC/execute don't say which
        self.cmd = bytearray()
        self.cmd_size = 0
        self.valid_init = False
        self.init = None
        self.fw_size = 0
        self.obj_size = 0
        self.off = self.crc = self.off_last = self.crc_last = 0
        self.flash = bytearray()
        self.app_valid = True  # an old app is on it
        self.flashed = None  # the new image, once activated
        self.replies = 0  # counts replies (for lose_reply / stray_before)
        self.writes = 0  # data write packets
        self.objects = 0  # data objects created
        self.uploads = 0  # data bytes written
        self.max_write = 0
        self.prn_set = None
        self.enter_calls = 0

    # -- transport
    def emit(self, data: bytes, delay=0.5):
        if not self.dtr:
            return  # app_usbd_cdc_acm_write: port not opened, reply dropped
        t = max([self.now] + [t for t, _ in self.out]) + delay
        enc = bytearray()
        for b in data:
            enc += bytes([ESC, ESC_END]) if b == END else bytes([ESC, ESC_ESC]) if b == ESC else bytes([b])
        self.out.append((t, bytes(enc) + bytes([END])))

    def reply(self, op, result, payload=b"", delay=0.5):
        self.replies += 1
        if self.stray_before == self.replies:
            self.emit(bytes([OP_RSP, OP_PING, SUCCESS, 0x77]), 0.1)  # an old ping answer
            self.emit(b"\x13\x37", 0.1)
        if self.garbage_before == self.replies:
            if self.dtr:
                self.out.append((self.now + 0.1, bytes([0x41, ESC, 0x42, 0x43, END, 0x99, 0x98, END])))
        if self.lose_reply == self.replies:
            return
        self.emit(bytes([OP_RSP, op, result]) + payload, delay)

    def read(self, mx, timeout):
        if self.gone:
            return -1
        if not self.out:
            self.now += timeout
            return b""
        t, data = self.out[0]
        if t > self.now + timeout:
            self.now += timeout
            return b""
        self.now = max(self.now, t)
        take = data[:mx]
        if len(take) < len(data):
            self.out[0] = (t, data[mx:])
        else:
            self.out.pop(0)
        return take

    def set_lines(self, dtr, rts):
        if self.gone:
            return -1
        self.dtr = dtr
        return 0

    def enter(self):
        self.enter_calls += 1
        if self.mode == "bl":
            return 0
        if not self.trigger:
            return -1
        # DETACH -> app pulls its own reset pin -> bootloader re-enumerates as 1915:521F (new port: DTR off)
        self.now += 1500
        self.dtr = False
        self.out.clear()
        if self.trigger_works:
            self.mode = "bl"
        return 0

    def write(self, data: bytes):
        if self.gone:
            return -1
        self.now += len(data) / 1000.0  # ~1 MB/s full speed
        if self.mode != "bl":
            return 0  # the app (an NCP) ignores DFU traffic
        for c in data:
            if c == END:
                pkt, bad = bytes(self.slip), self.junk
                self.slip, self.esc, self.junk = bytearray(), False, False
                if bad:
                    continue
                if not pkt:
                    self.violations.append("empty SLIP packet (the SDK would read length - 1)")
                    continue
                if len(pkt) > 1025 + 1:
                    self.violations.append(f"packet of {len(pkt)} bytes overflows the {1024}-byte RX buffer")
                    continue
                self.packet(pkt)
                if self.gone:
                    return -1
                continue
            if self.junk:
                continue
            if self.esc:
                self.esc = False
                if c == ESC_END:
                    self.slip.append(END)
                elif c == ESC_ESC:
                    self.slip.append(ESC)
                else:
                    self.junk = True
                continue
            if c == ESC:
                self.esc = True
                continue
            self.slip.append(c)
        return 0

    # -- the request handler
    def progress_reset(self):
        self.cmd = bytearray()
        self.off = self.crc = self.off_last = self.crc_last = self.obj_size = 0

    def packet(self, p: bytes):
        op, pl = p[0], p[1:]
        if op == OP_PING:
            self.reply(op, SUCCESS, pl[:1])
        elif op == OP_PRN:
            self.prn = self.prn_count = struct.unpack("<H", pl[:2])[0]
            self.prn_set = self.prn
            self.reply(op, SUCCESS)
        elif op == OP_MTU:
            self.reply(op, SUCCESS, struct.pack("<H", self.mtu))
        elif op == OP_SELECT:
            self.cur = pl[0]
            if pl[0] == 1:
                self.reply(op, SUCCESS, struct.pack("<III", self.init_max, len(self.cmd), zlib.crc32(bytes(self.cmd))))
            elif pl[0] == 2:
                self.reply(op, SUCCESS, struct.pack("<III", self.PAGE, self.off, self.crc))
            else:
                self.reply(op, INVALID_OBJECT)
        elif op == OP_CREATE:
            typ, size = pl[0], struct.unpack("<I", pl[1:5])[0]
            self.cur = typ
            self.prn_count = self.prn
            if typ == 1:
                if size == 0:
                    return self.reply(op, INVALID_PARAM)
                if size > self.init_max:
                    return self.reply(op, NO_RESOURCES)
                self.valid_init = False
                self.progress_reset()
                self.cmd_size = size
                self.reply(op, SUCCESS)
            elif typ == 2:
                self.objects += 1
                if self.gone_at_object == self.objects:
                    self.gone = True
                    return
                if not self.valid_init:
                    return self.reply(op, NOT_PERMITTED)
                if size == 0:
                    return self.reply(op, INVALID_PARAM)
                if size % self.PAGE and self.off_last + size != self.fw_size:
                    return self.reply(op, INVALID_PARAM)
                if size > self.PAGE:
                    return self.reply(op, NO_RESOURCES)
                if self.off_last + size > self.fw_size:
                    return self.reply(op, NOT_PERMITTED)
                self.obj_size = size
                self.crc, self.off = self.crc_last, self.off_last
                del self.flash[self.off:]
                self.reply(op, SUCCESS, delay=self.erase_ms * ((size + self.PAGE - 1) // self.PAGE))
            else:
                self.reply(op, 7)
        elif op == OP_WRITE:
            self.max_write = max(self.max_write, len(pl))
            if self.cur == 2:
                self.writes += 1
                obj_off = self.off - self.off_last
                if not self.valid_init:
                    result = NOT_PERMITTED
                elif len(pl) + obj_off > self.obj_size:
                    result = INVALID_PARAM
                elif self.writes in self.drop_writes:
                    result = SUCCESS  # nrf_dfu_flash_store queue full: not stored, no reply with PRN 0
                else:
                    data = bytearray(pl)
                    if self.writes in self.flip_writes or self.flip_all_in_object == self.off_last // self.PAGE:
                        data[len(data) // 2] ^= 0x10  # damaged on the way in
                    self.crc = zlib.crc32(bytes(data), self.crc)
                    self.flash += data
                    self.off += len(data)
                    self.uploads += len(data)
                    result = SUCCESS
            else:
                if len(self.cmd) + len(pl) > self.cmd_size:
                    result = INVALID_PARAM
                else:
                    self.cmd += pl
                    result = SUCCESS
            if self.prn:
                self.prn_count -= 1
                if self.prn_count == 0:
                    self.prn_count = self.prn
                    o, c = (self.off, self.crc) if self.cur == 2 else (len(self.cmd), zlib.crc32(bytes(self.cmd)))
                    self.reply(OP_CRC, SUCCESS, struct.pack("<II", o, c))
            if result != SUCCESS:
                self.violations.append(f"write refused ({result})")
        elif op == OP_CRC:
            if self.cur == 2:
                self.reply(op, SUCCESS, struct.pack("<II", self.off, self.crc))
            else:
                self.reply(op, SUCCESS, struct.pack("<II", len(self.cmd), zlib.crc32(bytes(self.cmd))))
        elif op == OP_EXECUTE:
            if self.cur == 2:
                self.execute_data()
            else:
                self.execute_command()
        else:
            self.reply(op, NOT_SUPPORTED)

    def execute_command(self):
        if len(self.cmd) != self.cmd_size:
            return self.reply(OP_EXECUTE, NOT_PERMITTED)
        if self.valid_init:
            return self.reply(OP_EXECUTE, SUCCESS)
        ic = decode_init(bytes(self.cmd))
        if ic is None:
            return self.reply(OP_EXECUTE, INVALID_OBJECT)
        ext = self.prevalidate(ic)
        if ext is None:
            if "app_size" not in ic or ic.get("type") != 0:
                ext = EXT_INIT_INVALID
        if ext is not None:
            return self.reply(OP_EXECUTE, EXT, bytes([ext]))
        if ic["app_size"] > self.app_max:
            self.progress_reset()
            return self.reply(OP_EXECUTE, NO_RESOURCES)
        self.valid_init, self.init, self.fw_size = True, ic, ic["app_size"]
        self.app_valid = False  # single bank: the old app goes
        self.flash = bytearray()
        self.reply(OP_EXECUTE, SUCCESS)

    def prevalidate(self, ic):
        """nrf_dfu_validation_prevalidate + nrf_dfu_ver_validation_check (open bootloader config)."""
        if self.require_signed and ic.get("type") == 0 and not ic["signed"]:
            return EXT_SIG_MISSING
        if self.require_signed and ic["signed"]:
            return EXT_SIG_MISSING  # (a real one would check it; ours has no key: treat as refused)
        if "type" not in ic or ic["type"] not in (0, 1, 2, 3):
            return EXT_INIT_INVALID
        if ic.get("hash_type") != 3:
            return EXT_HASH_TYPE
        if "hw_version" not in ic:
            return EXT_INIT_INVALID
        if ic["hw_version"] != self.hw:
            return EXT_HW
        sd = ic["sd_req"]
        if self.sd_present:
            ok = bool(sd) and (sd[0] == 0 and ic["type"] == 0 or self.sd_fwid in sd)
        else:
            ok = not (sd and sd[0] != 0)
        if not ok:
            return EXT_SD
        return None

    def execute_data(self):
        if self.obj_size != self.off - self.off_last:
            return self.reply(OP_EXECUTE, NOT_PERMITTED)
        self.obj_size = 0
        self.crc_last, self.off_last = self.crc, self.off
        if self.off != self.fw_size:
            return self.reply(OP_EXECUTE, SUCCESS)
        image = bytearray(self.flash[:self.fw_size])
        if self.flash_error:
            image[100] ^= 1
        if hashlib.sha256(bytes(image)).digest()[::-1] != self.init["hash"]:
            return self.reply(OP_EXECUTE, EXT, bytes([EXT_VERIFY]), delay=40)
        self.reply(OP_EXECUTE, SUCCESS, delay=40)
        self.flashed = bytes(image)
        self.app_valid = True
        self.mode = "booted"


# ---------------------------------------------------------------- driving the harness

def wsl(p: Path) -> str:
    if sys.platform != "win32":  # Linux (CI): run_all.py puts linux/wsl in front, paths stay as they are
        return p.resolve().as_posix()
    s = str(p).replace("\\", "/")
    return f"/mnt/{s[0].lower()}{s[2:]}"


def build():
    cmd = (f"gcc -std=gnu11 -O1 -g -Wall -Wextra -Werror -fsanitize=address,undefined -fno-sanitize-recover=all "
           f"-I{wsl(MAIN)} {wsl(HERE / 'nrfdfu_harness.c')} {wsl(MAIN / 'nrfdfu_core.c')} -o {EXE}")
    r = subprocess.run(["wsl", "-d", "Ubuntu-24.04", "--", "bash", "-c", cmd], capture_output=True, text=True)
    if r.returncode:
        print(r.stdout, r.stderr)
        sys.exit("build failed")


def run(args, image: bytes, dongle: Dongle | None):
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
            d = dongle.read(mx, to)
            if d == -1:
                answer(-1)
            else:
                answer(len(d), d)
        elif op == b"W":
            (n,) = struct.unpack("<I", need(4))
            answer(dongle.write(need(n)))
        elif op == b"L":
            d, r = need(2)
            answer(dongle.set_lines(bool(d), bool(r)))
        elif op == b"S":
            (ms,) = struct.unpack("<I", need(4))
            if dongle:
                dongle.now += ms
            answer(0)
        elif op == b"T":
            answer(int(dongle.now) if dongle else 0)
        elif op == b"E":
            answer(dongle.enter())
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
            p.wait(timeout=60)
            err = p.stderr.read().decode(errors="replace")
            if p.returncode != 0 or "runtime error" in err or "AddressSanitizer" in err or "LeakSanitizer" in err:
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


def flash(name, image, dongle, *, dat=None, fw=1, expect_ok=True, err_has=None, extra=None, mode="flash"):
    if mode == "flash":
        res = run(["flash", dat.hex() if dat is not None else "-", fw], image, dongle)
    else:
        res = run(["hexflash", fw], image, dongle)
    desc = f"{res.get('err')} | {res['logs'][-3:]}"
    ok = bool(res.get("ok")) == expect_ok
    if err_has:
        ok &= err_has.lower() in (res.get("err") or "").lower()
    ok &= not dongle.violations
    if dongle.violations:
        desc += f" | violations {dongle.violations[:3]}"
    if expect_ok and ok:
        want = image if mode == "flash" else hex_reference(image.decode())
        ok &= dongle.flashed == want and dongle.mode == "booted" and not res["app_erased"]
        if not ok:
            desc += f" | flashed {len(dongle.flashed or b'')} vs {len(want)}, mode {dongle.mode}"
    if extra:
        e = extra(res, dongle)
        if e:
            ok = False
            desc += f" | {e}"
    check(f"flash: {name}", ok, desc)
    return res


def untouched(r, d):
    return None if d.app_valid and d.uploads == 0 and not r["app_erased"] else f"app_valid {d.app_valid} uploads {d.uploads}"


def stuck_in_bl(r, d):
    return None if r["app_erased"] and d.mode == "bl" and not d.app_valid else f"app_erased {r['app_erased']} mode {d.mode}"


def parse(name, dat, image, *, parse_has=None, policy_has=None, image_has=None, extra=None):
    res = run(["parse", dat.hex() if dat else "x"], image, None)
    ok = True
    for key, want in (("parse", parse_has), ("policy", policy_has), ("image", image_has)):
        if want is None:
            ok &= res[key] == ""
        else:
            ok &= want.lower() in res[key].lower()
    desc = f"parse={res['parse']!r} policy={res['policy']!r} image={res['image']!r}"
    if extra and ok:
        e = extra(res)
        if e:
            ok = False
            desc += f" | {e}"
    check(f"init: {name}", ok, desc)
    return res


def hexbin(name, text: str, *, err_has=None, extra=None):
    res = run(["hexbin"], text.encode(), None)
    if err_has:
        ok = err_has.lower() in res["hexerr"].lower()
    else:
        ref = hex_reference(text)
        ok = res["hexerr"] == "" and res["len"] == len(ref) and res["sha"] == hashlib.sha256(ref).hexdigest()
    desc = f"{res['hexerr']} len {res.get('len')}"
    if extra and ok:
        e = extra(res)
        if e:
            ok = False
            desc += f" | {e}"
    check(f"hex: {name}", ok, desc)
    return res


def main():
    build()

    # ---- init packets: byte-identical to pc-nrfutil
    r7 = random.Random(7)
    ga = bytes(r7.getrandbits(8) for _ in range(5000))
    gb = bytes(range(256)) * 1000 + b"\x01\x02\x03\x04"
    for name, img, fw, gold in (("A", ga, 1, GOLDEN_A), ("B", gb, 0x12345678, GOLDEN_B)):
        res = run(["build", fw, 52], img, None)
        check(f"init: built == pc-nrfutil golden {name}", res["init"] == gold, f"{res['init']}\n     {gold}")
    check("init: python builder == golden (cross-check)", make_init(ga).hex() == GOLDEN_A, make_init(ga).hex())

    img = app_image(370_000)
    good = make_init(img)
    parse("golden decodes", bytes.fromhex(GOLDEN_A), ga, image_has="vector table",  # random bytes: no vectors
          extra=lambda r: None if r["fw"] == 1 and r["hw"] == 52 and r["app_size"] == 5000 and r["sd_req"] == [0] and
          r["hash"] == hashlib.sha256(ga).digest()[::-1].hex() and not r["signed"] else r)
    parse("good dongle app", good, img)
    parse("signed command (open bootloader ignores it)", make_init(img, signed=True), img,
          extra=lambda r: None if r["signed"] else r)
    parse("unpacked sd_req", make_init(img, packed=False), img)
    parse("command and signed command both", make_init(img, signed=True, both=True), img, parse_has="both")
    parse("SoftDevice update refused", make_init(img, fw_type=1), img, policy_has="SoftDevice update")
    parse("bootloader update refused", make_init(img, fw_type=2), img, policy_has="bootloader update")
    parse("no type", make_init(img, drop=(4,)), img, policy_has="no firmware type")
    parse("nrfutil without --hw-version (0xFFFFFFFF)", make_init(img, hw=0xFFFFFFFF), img, policy_has="hw-version 52")
    parse("no hw_version", make_init(img, drop=(2,)), img, policy_has="no hw_version")
    parse("CRC hash type", make_init(img, hash_type=1), img, policy_has="SHA-256")
    parse("no app_size", make_init(img, drop=(7,)), img, policy_has="application size")
    parse("17 sd_req entries", make_init(img, sd_req=tuple(range(1, 18))), img, parse_has="16 sd_req")
    parse("cut short", good[:-5], img, parse_has="cut short")
    parse("empty", b"", img, parse_has="empty")
    parse("a .bin is not a .dat", img[:300], img, parse_has="init packet")
    parse("no init inside", fld(1, 2, fld(1, 0, 1)), img, parse_has="no init command")
    parse("hash over 32 bytes", fld(1, 2, fld(1, 0, 1) + fld(2, 2, fld(8, 2, fld(1, 0, 3) + fld(2, 2, bytes(33))))), img,
          parse_has="32 bytes")
    parse("size differs from the .bin", make_init(img, app_size=len(img) - 4), img, image_has="different packages")
    other = bytearray(img)
    other[5000] ^= 1
    parse("hash differs from the .bin", make_init(img), bytes(other), image_has="SHA-256")
    sdapp = app_image(100_000, reset=(0x27000 + 0x200) | 1)
    parse("linked after a SoftDevice, says sd_req 0", make_init(sdapp), sdapp, image_has="isn't linked")
    parse("linked after a SoftDevice, says it needs S140", make_init(sdapp, sd_req=(0xCA,)), sdapp)
    nosp = app_image(10_000, sp=0x00001234)
    parse("no vector table", make_init(nosp), nosp, image_has="vector table")
    arm = app_image(10_000, reset=0x1200)
    parse("reset handler not Thumb", make_init(arm), arm, image_has="isn't linked")

    # ---- Intel HEX
    a1 = app_image(70_000, seed=4)
    hexbin("plain, one block", ihex([(0x1000, a1)]))
    hexbin("gaps filled with 0xFF, odd length rounded to a word", ihex([(0x1000, a1[:30000]), (0x9000, b"\x11" * 33)]),
           extra=lambda r: None if r["len"] % 4 == 0 and r["start"] == 0x1000 else r)
    hexbin("UICR records dropped like nrfutil", ihex([(0x1000, a1)], uicr=b"\x05\x00\x00\x00"),
           extra=lambda r: None if r["dropped"] == 4 else r)
    hexbin("records out of order", ihex([(0x1000, a1)], shuffle=True))
    hexbin("type-02 segment addresses", ihex([(0x1000, a1[:20000])], seg=True))
    hexbin("32-byte records crossing 64 KB", ihex([(0xF000, a1)], width=32))
    hexbin("MBR data refused", ihex([(0x0, a1[:5000])]), err_has="MBR")
    hexbin("bootloader area refused", ihex([(0xDF000, a1[:8192])]), err_has="bootloader area")
    hexbin("bad checksum", ihex([(0x1000, a1[:64])], bad_sum=True), err_has="checksum")
    hexbin("no end record", ihex([(0x1000, a1[:64])], eof=False), err_has="cut short")
    hexbin("not hex at all", "hello\r\n", err_has="not an Intel HEX")
    hexbin("bad character", ":10100000ZZ\r\n", err_has="bad character")
    hexbin("only an end record", ":00000001FF\r\n", err_has="no firmware data")
    reals = []
    for real in sys.argv[1:]:
        text = Path(real).read_text()
        res = hexbin(f"real file {Path(real).name}", text)
        if res["hexerr"] == "":
            reals.append((Path(real).name, text))

    # ---- flashing
    flash("happy path, 370 KB, already in the bootloader", img, Dongle(),
          extra=lambda r, d: None if r["entered"] == "already in the bootloader" and r["chunk"] == 1024 and
          r["obj_max"] == 4096 and r["retries"] == 0 and d.prn_set == 0 and d.max_write == 1024 else r)
    flash("init packet from the zip (pc-nrfutil bytes)", img, Dongle(), dat=good)
    flash("signed init packet on the open bootloader", img, Dongle(), dat=make_init(img, signed=True))
    flash("app running, DFU trigger (DTR set again on the new port)", img, Dongle(mode="app", trigger=True),
          extra=lambda r, d: None if r["entered"] == "DFU trigger" and d.enter_calls == 1 else r["entered"])
    flash("app running, no DFU trigger (NCS build): press RESET", img, Dongle(mode="app"), expect_ok=False,
          err_has="press its RESET button", extra=untouched)
    flash("trigger sent, bootloader never shows", img, Dongle(mode="app", trigger=True, trigger_works=False),
          expect_ok=False, err_has="didn't come up", extra=untouched)
    flash("UART-sized MTU (131)", img[:50_000], Dongle(mtu=131),
          extra=lambda r, d: None if r["chunk"] == 64 and d.max_write == 64 else r)
    flash("old bootloader with a 64-byte init buffer", img, Dongle(init_max=64), expect_ok=False, err_has="at most 64",
          extra=untouched)
    flash("bootloader of another board (hw_version 53)", img, Dongle(hw=53), expect_ok=False,
          err_has="hardware version", extra=lambda r, d: untouched(r, d) or (None if r["ext"] == 6 else r["ext"]))
    flash("secure bootloader (signed updates only)", img, Dongle(require_signed=True), expect_ok=False,
          err_has="wants signed updates", extra=untouched)
    flash("needs S140, dongle has no SoftDevice", sdapp, Dongle(), dat=make_init(sdapp, sd_req=(0xCA,)), expect_ok=False,
          err_has="SoftDevice", extra=untouched)
    flash("needs S140, dongle has it", sdapp, Dongle(sd_present=True), dat=make_init(sdapp, sd_req=(0xCA,)))
    flash("sd_req 0 on a dongle with a SoftDevice (app replaces it)", img, Dongle(sd_present=True))
    big = app_image(0xDD000)
    flash("no room for it", big, Dongle(), expect_ok=False, err_has="insufficient resources", extra=untouched)
    flash("dropped writes (RX pool full): objects resent", img, Dongle(drop_writes={5, 77, 200}),
          extra=lambda r, d: None if r["retries"] == 3 else r["retries"])
    flash("bit flips on the way in: objects resent", img, Dongle(flip_writes={1, 150, 151}),
          extra=lambda r, d: None if r["retries"] >= 2 else r["retries"])
    flash("one object always damaged: gives up, waits in the bootloader", img, Dongle(flip_all_in_object=9),
          expect_ok=False, err_has="kept arriving damaged at 9%", extra=stuck_in_bl)
    flash("a reply lost", img, Dongle(lose_reply=40), expect_ok=False, err_has="didn't answer", extra=stuck_in_bl)
    flash("stray frames before a reply", img, Dongle(stray_before=30))
    flash("SLIP garbage before a reply", img, Dongle(garbage_before=25))
    flash("stick pulled mid-upload", img, Dongle(gone_at_object=40), expect_ok=False, err_has="went away",
          extra=lambda r, d: None if r["app_erased"] else r)
    flash("flash error caught by the bootloader's SHA-256", img, Dongle(flash_error=True), expect_ok=False,
          err_has="doesn't match the hash", extra=lambda r, d: None if r["app_erased"] and r["ext"] == 0x0C else r)
    flash("init packet for another file: dongle never touched", img, Dongle(), dat=make_init(app_image(370_000, seed=9)),
          expect_ok=False, err_has="SHA-256", extra=lambda r, d: None if not d.out and d.uploads == 0 and d.dtr is False else "touched")
    flash("bootloader update: dongle never touched", img, Dongle(), dat=make_init(img, fw_type=2), expect_ok=False,
          err_has="only applications", extra=lambda r, d: None if d.dtr is False else "touched")
    tiny = app_image(100, reset=0x1041)
    flash("tiny image (100 bytes, one short object)", tiny, Dongle())
    exact = app_image(4096 * 20)
    flash("exactly 20 pages", exact, Dongle(), extra=lambda r, d: None if d.objects == 20 else d.objects)
    flash("progress reported", img, Dongle(),
          extra=lambda r, d: None if r["progress"][-1] == ("upload", len(img), len(img)) and len(r["progress"]) == 91
          else r["progress"][-2:])
    hx = ihex([(0x1000, app_image(120_000, seed=5)), (0x20000, b"\x22" * 1001)], uicr=b"\x01\x02\x03\x04")
    flash("Intel HEX end to end (gap, UICR dropped)", hx.encode(), Dongle(), mode="hex")
    for name, text in reals:
        flash(f"real file {name} end to end", text.encode(), Dongle(), mode="hex")

    print(f"\n{len(FAILS)} failed" if FAILS else "\nall passed")
    sys.exit(1 if FAILS else 0)


if __name__ == "__main__":
    main()
