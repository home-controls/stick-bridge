"""Drives the firmware's own TI dongle flasher (main/tiflash_core.c) and Intel HEX parser (main/ihex_core.c),
built for the host by tiflash_harness.c with ASan + UBSan in WSL, against a simulated CC26x2 on a virtual clock:

- the board: a SONOFF ZBDongle-P style gated DTR/RTS circuit, or DTR/RTS wired straight to the BSL pin and
  !RESET (optionally swapped); the ROM looks at the backdoor pin 1 ms after reset is released, so only the
  matching pattern gets it into the bootloader;
- the ROM bootloader: autobaud on 0x55 0x55, ACK/NACK, packet checksums, GET_STATUS codes, GET_CHIP_ID,
  MEMORY_READ (ICEPICK, FLASH_SIZE, flash), BANK_ERASE / SECTOR_ERASE honouring the CCFG it booted with
  (bank erase disabled, write-protected sectors), DOWNLOAD + SEND_DATA with NOR-flash programming, CRC32,
  RESET;
- faults: NACKed, corrupted and short (lost byte) packets, lost ACKs, a chip that never syncs, one that
  can't autobaud at 460800, a stuck flash bit (CRC mismatch), a CRC that's wrong once, no MEMORY_READ, the
  USB stick pulled mid-write.

Plus the Intel HEX parser against good and broken files fed in random pieces, and (when the network allows)
a real Koenkk Z-Stack coordinator .hex parsed by ihex_core and flashed into the simulator.

  uv run --python 3.12.15 --no-project test_tiflash.py
"""
import io
import json
import random
import struct
import subprocess
import sys
import tempfile
import urllib.request
import zipfile
import zlib
from pathlib import Path

HERE = Path(__file__).resolve().parent
MAIN = HERE.parent.parent / "main"
EXE = "/tmp/tiflash_harness"

K352, K704 = 0x58000, 0xB0000
SECTOR = 8192
CCFG_LEN = 88
ACK, NACK = 0xCC, 0x33
REAL_URL = ("https://github.com/Koenkk/Z-Stack-firmware/releases/download/Z-Stack_3.x.0_coordinator_20250321/"
            "CC1352P2_CC2652P_launchpad_coordinator_20250321.zip")

# The CCFG of Koenkk's CC1352P2_CC2652P_launchpad_coordinator_20250321.hex, word by word (offset 0x00..0x54).
REAL_CCFG = [0x01800000, 0xFFC00010, 0x0058FFFD, 0xFDB9C1FF, 0xFFFFFFFF, 0xFFFFFFFF, 0xFFFFFFFF, 0xFFFFFFFF,
             0xFFFFFFFF, 0xFFFFFFFF, 0xFFFFFFFF, 0xFFFFFFFF, 0xC5FE0FC5, 0xFFFFFFFF, 0xFFFFFF00, 0xFFC5C500,
             0xFF000000, 0x00000000, 0xFFFFFFFF, 0xFFFFFFFF, 0xFFFFFFFF, 0xFFFFFFFF]


def bl_config(pin=15, level=0, boot=0xC5, backdoor=0xC5):
    return (boot << 24) | (0x7F << 17) | (level << 16) | (pin << 8) | backdoor


# ---------------------------------------------------------------- images

def make_image(size=K352, *, bl=None, erase_conf=0xFFFFFFFF, prot=None, image_valid=0, seed=1, sp=0x20013000,
               rv=0x00002001, blank_ccfg=False) -> bytes:
    """A flat image: code with all-0xFF holes of awkward sizes, then the CCFG in the last 88 bytes."""
    rnd = random.Random(seed)
    img = bytearray(b"\xff" * size)

    def fill(a, n):
        img[a:a + n] = bytes(rnd.getrandbits(8) for _ in range(n))

    fill(0, 0x9000)                 # code
    fill(0x9000 + 3 * 248 + 4, 1000)  # after a hole of exactly 3 chunks + 4 bytes
    img[0xC000 + 100] = 0x12        # one byte in an otherwise blank chunk
    fill(0x10000, 0x6123)           # odd-sized run
    fill(0x20000 - 8, 16)           # straddles a sector boundary
    fill(0x30004, 4)
    struct.pack_into("<II", img, 0, sp, rv)
    if not blank_ccfg:
        c = list(REAL_CCFG)
        c[12] = bl if bl is not None else bl_config()
        c[13] = erase_conf
        c[17] = image_valid
        if prot:
            c[18:22] = prot
        struct.pack_into("<22I", img, size - CCFG_LEN, *c)
    return bytes(img)


# ---------------------------------------------------------------- the simulated chip

class Chip:
    def __init__(self, *, flash_size=K352, board="sonoff", board_invert=False, wired_pin=15, wired_level=0,
                 flash=None, start="app", never_sync=False, max_baud=None, nack_every=None, corrupt_every=None,
                 drop_every=None, lose_ack_every=None, stuck=None, crc_lie_once=False, mem_read_ok=True,
                 gone_at_chunk=None, wafer=0xBB41, pg=3, seed=0):
        self.now = 0.0
        self.host_baud = None
        self.out = []      # (t, byte, baud)
        self.events = []   # (t, seq, fn)
        self.evseq = 0
        self.size = flash_size
        self.flash = bytearray(flash if flash is not None else make_image(flash_size, seed=99,
                                                                        bl=bl_config(wired_pin, wired_level)))
        self.board, self.board_invert = board, board_invert
        self.wired_pin, self.wired_level = wired_pin, wired_level
        self.lines = (False, False)
        self.line_changes = 0
        self.mode = {"bsl": "sync", "synced": "bsl"}.get(start, "app")
        self.never_sync, self.max_baud = never_sync, max_baud
        self.nack_every, self.corrupt_every, self.drop_every = nack_every, corrupt_every, drop_every
        self.lose_ack_every, self.stuck, self.crc_lie_once = lose_ack_every, stuck, crc_lie_once
        self.mem_read_ok, self.gone_at_chunk = mem_read_ok, gone_at_chunk
        self.wafer, self.pg = wafer, pg
        self.gone = False
        self.baud = 460800 if start == "synced" else None  # what the autobaud locked to
        self.prev55 = False
        self.rx = bytearray()
        self.host_ack_left = 0  # bytes of the host's ACK/NACK still expected after we sent a packet
        self.status = 0x40
        self.dl_addr = self.dl_left = 0
        self.boot_gen = 0
        self.sd_seen = 0
        self.cmds = []
        self.erase_order = []  # sectors, in the order SECTOR_ERASE asked for them
        self.entries = 0      # times the ROM started the bootloader
        self.boots_app = 0
        self.lied = False
        self.latch()

    # ---- time and delivery
    def at(self, dt, fn):
        self.evseq += 1
        self.events.append((self.now + dt, self.evseq, fn))
        self.events.sort()

    def emit(self, data: bytes, delay=0.2):
        baud = self.baud or 115200
        t = max(self.now + delay, self.out[-1][0] if self.out else 0)
        for b in data:
            t += 10000.0 / baud
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
                if baud == self.host_baud:
                    got.append(b)
            if got:
                return bytes(got)
            if self.now >= deadline:
                return b""
            t = min([deadline] + ([self.out[0][0]] if self.out else []) + ([self.events[0][0]] if self.events else []))
            self.now = max(self.now, t)

    # ---- the board's DTR/RTS wiring (True = asserted, pin driven low)
    def pins(self):
        """(reset held low, BSL pin level)"""
        dtr, rts = self.lines
        if self.board == "sonoff":  # the table in cc2538_bsl.py, read as pin levels
            return (not dtr) and rts, 0 if (dtr and not rts) else 1
        if self.board == "direct":
            bsl, rst = (rts, dtr) if self.board_invert else (dtr, rts)
            return rst, 0 if bsl else 1
        return False, 1  # no circuit

    def set_lines(self, dtr, rts):
        if self.gone:
            return -1
        self.line_changes += 1
        was_reset, _ = self.pins()
        self.lines = (dtr, rts)
        now_reset, _ = self.pins()
        if now_reset and not was_reset:
            self.boot_gen += 1
            self.mode = "reset"
            self.out.clear()
        elif was_reset and not now_reset:
            self.boot_gen += 1
            g = self.boot_gen
            self.at(1.0, lambda: self.boot() if self.boot_gen == g and not self.pins()[0] else None)
        return 0

    def ccfg_word(self, off):
        return struct.unpack_from("<I", self.flash, self.size - CCFG_LEN + off)[0]

    def latch(self):  # the CCFG settings the chip applies at boot
        self.erase_conf = self.ccfg_word(0x34)
        self.prot = [self.ccfg_word(0x48 + 4 * i) for i in range(4)]

    def boot(self):
        self.latch()
        self.rx.clear()
        self.host_ack_left = 0
        self.baud = None
        self.prev55 = False
        self.dl_left = 0
        self.status = 0x40
        bl = self.ccfg_word(0x30)
        valid = self.ccfg_word(0x44) == 0
        backdoor = (bl >> 24) == 0xC5 and (bl & 0xFF) == 0xC5
        pin, level = (bl >> 8) & 0xFF, (bl >> 16) & 1
        _, bsl_level = self.pins()
        if not valid or (backdoor and pin == self.wired_pin and level == bsl_level):
            self.mode = "sync"
            self.entries += 1
        else:
            self.mode = "app"
            self.boots_app += 1

    # ---- bytes from the host
    def write(self, data: bytes):
        if self.gone:
            return -1
        self.now += len(data) * 10000.0 / (self.host_baud or 115200)
        self.run_events(self.now)
        if self.mode == "sync":
            if self.never_sync or (self.max_baud and self.host_baud > self.max_baud):
                return 0
            for b in data:
                if b == 0x55 and self.prev55 and self.mode == "sync":
                    self.baud = self.host_baud
                    self.mode = "bsl"
                    self.emit(bytes([0, ACK]))
                    continue
                if self.mode == "bsl":
                    self.byte(b)
                else:
                    self.prev55 = b == 0x55
            return 0
        if self.mode != "bsl" or self.host_baud != self.baud:
            return 0
        flags = {}
        if len(data) >= 3 and data[0] == len(data) and data[2] == 0x24:  # a SEND_DATA packet: faults go here
            self.sd_seen += 1
            n = self.sd_seen
            if self.gone_at_chunk and n >= self.gone_at_chunk:
                self.gone = True
                return 0
            if self.corrupt_every and n % self.corrupt_every == 0:
                data = data[:10] + bytes([data[10] ^ 0x08]) + data[11:]
            if self.drop_every and n % self.drop_every == 0:
                i = next(k for k in range(3, len(data)) if data[k])  # drop a non-zero byte
                data = data[:i] + data[i + 1:]
            if self.nack_every and n % self.nack_every == 0:
                flags["nack"] = True
            if self.lose_ack_every and n % self.lose_ack_every == 0:
                flags["lose_ack"] = True
        for b in data:
            self.byte(b, flags)
        return 0

    def byte(self, b, flags=None):
        if self.host_ack_left:
            self.host_ack_left -= 1
            return
        if not self.rx and b == 0:
            return  # padding between packets
        self.rx.append(b)
        if self.rx[0] < 3 or len(self.rx) >= self.rx[0]:
            p, self.rx = bytes(self.rx), bytearray()
            self.packet(p, flags or {})

    def send_packet(self, data: bytes):
        self.emit(bytes([len(data) + 2, sum(data) & 0xFF]) + data)
        self.host_ack_left = 2

    def packet(self, p, flags):
        if len(p) < 3 or (sum(p[2:]) & 0xFF) != p[1] or flags.get("nack"):
            self.emit(bytes([0, NACK]))
            return
        cmd, pl = p[2], p[3:]
        self.cmds.append(cmd)
        delay = 50.0 if cmd == 0x2C else 8.0 if cmd == 0x26 else 0.2
        if not flags.get("lose_ack"):
            self.emit(bytes([0, ACK]), delay=delay)
        a = struct.unpack(">I", pl[:4])[0] if len(pl) >= 4 else 0
        if cmd == 0x20:
            self.status = 0x40
        elif cmd == 0x23:
            self.send_packet(bytes([self.status]))
        elif cmd == 0x28:
            self.send_packet(bytes([0x30, 0x00, 0xBB, 0x41]))  # arbitrary: the CC26x2 ROM doesn't document it
            self.status = 0x40
        elif cmd == 0x2A:
            if not self.mem_read_ok or len(pl) != 6:
                self.status = 0x41
                return
            out = b"".join(struct.pack("<I", self.mem32(a + 4 * i)) for i in range(pl[5]))
            self.send_packet(out)
            self.status = 0x40
        elif cmd == 0x2C:
            if not (self.erase_conf & 1):
                self.status = 0x42
                return
            for s in range(self.size // SECTOR):
                self.erase_sector(s)
            self.status = 0x40
        elif cmd == 0x26:
            if len(pl) != 4 or a % SECTOR or a >= self.size:
                self.status = 0x43
                return
            self.erase_sector(a // SECTOR)
            self.status = 0x40
        elif cmd == 0x21:
            size = struct.unpack(">I", pl[4:8])[0] if len(pl) == 8 else 0
            if len(pl) != 8 or size % 4 or size == 0 or a + size > self.size:
                self.dl_left = 0
                self.status = 0x43
                return
            self.dl_addr, self.dl_left = a, size
            self.status = 0x40
        elif cmd == 0x24:
            n = len(pl)
            if self.dl_left == 0 or n > self.dl_left or n % 4:
                self.status = 0x42
                return
            for i, v in enumerate(pl):
                ad = self.dl_addr + i
                if self.stuck and self.stuck[0] == ad:
                    v &= self.stuck[1]
                self.flash[ad] &= v  # NOR flash: programming only clears bits
            self.dl_addr += n
            self.dl_left -= n
            self.status = 0x40
        elif cmd == 0x27:
            size = struct.unpack(">I", pl[4:8])[0]
            crc = zlib.crc32(bytes(self.flash[a:a + size]))
            if self.crc_lie_once and not self.lied:
                crc ^= 0x10
                self.lied = True
            self.send_packet(struct.pack(">I", crc))
            self.status = 0x40
        elif cmd == 0x25:
            self.mode = "reset"
            self.at(1.0, self.boot)
        else:
            self.status = 0x41

    def erase_sector(self, s):
        self.erase_order.append(s)
        if self.prot[s // 32] & (1 << (s % 32)):
            self.flash[s * SECTOR:(s + 1) * SECTOR] = b"\xff" * SECTOR

    def mem32(self, a):
        if a == 0x50001318:  # ICEPICK_DEVICE_ID: PG_REV | WAFER_ID | manufacturer
            return (self.pg << 28) | (self.wafer << 12) | 0x02F
        if a == 0x4003002C:  # FLASH_SIZE.SECTORS
            return 0xFFFF0000 | (self.size // SECTOR)
        if 0 <= a < self.size:
            return struct.unpack_from("<I", self.flash, a)[0]
        return 0


# ---------------------------------------------------------------- driving the harness

def wsl(p: Path) -> str:
    if sys.platform != "win32":  # Linux (CI): run_all.py puts linux/wsl in front, paths stay as they are
        return p.resolve().as_posix()
    s = str(p).replace("\\", "/")
    return f"/mnt/{s[0].lower()}{s[2:]}"


def build():
    cmd = (f"gcc -std=gnu11 -O1 -g -Wall -Wextra -Werror -fsanitize=address,undefined -fno-sanitize-recover=all "
           f"-I{wsl(MAIN)} {wsl(HERE / 'tiflash_harness.c')} {wsl(MAIN / 'tiflash_core.c')} {wsl(MAIN / 'ihex_core.c')} "
           f"-o {EXE}")
    r = subprocess.run(["wsl", "-d", "Ubuntu-24.04", "--", "bash", "-c", cmd], capture_output=True, text=True)
    if r.returncode:
        print(r.stdout, r.stderr)
        sys.exit("build failed")


def run(args, image: bytes, chip: Chip | None, on_data=None):
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
            d = chip.read(mx, to)
            if d == -1:
                answer(-1)
            else:
                answer(len(d), d)
        elif op == b"W":
            (n,) = struct.unpack("<I", need(4))
            answer(chip.write(need(n)))
        elif op == b"B":
            (b,) = struct.unpack("<I", need(4))
            chip.host_baud = b
            answer(-1 if chip.gone else 0)
        elif op == b"L":
            d, r = need(2)
            answer(chip.set_lines(bool(d), bool(r)))
        elif op == b"S":
            (ms,) = struct.unpack("<I", need(4))
            if chip:
                target = chip.now + ms
                chip.run_events(target)
                chip.now = max(chip.now, target)
            answer(0)
        elif op == b"T":
            answer(int(chip.now) if chip else 0)
        elif op == b"I":
            off, n = struct.unpack("<II", need(8))
            if off + n > len(image):
                answer(-1)
            else:
                answer(0, image[off:off + n])
        elif op == b"D":
            addr, n = struct.unpack("<II", need(8))
            answer(on_data(addr, need(n)) if on_data else 0)
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


ENTRY = {"none": 0, "sonoff": 1, "default": 2}
DESTRUCTIVE = {0x2C, 0x26, 0x21, 0x24}


def flash(name, image, chip, *, entry="sonoff", invert=False, active_high=False, skip_size=False, allow_pin=False,
          sector=False, no_reset=False, baud=0, fallback=0, chip_flash=0, expect_ok=True, err_has=None, extra=None):
    flags = invert * 1 | active_high * 2 | skip_size * 4 | allow_pin * 8 | sector * 16 | no_reset * 32
    before = bytes(chip.flash)
    res = run(["flash", ENTRY[entry], flags, baud, fallback, chip_flash], image, chip)
    chip.run_events(chip.now + 100)  # let a RESET finish booting
    chip.now += 100
    desc = f"{res['err']} | {res['logs'][-3:]}"
    ok = bool(res["ok"]) == expect_ok
    if err_has:
        ok &= err_has.lower() in res["err"].lower()
    if expect_ok and ok:
        ok &= bytes(chip.flash) == image and res["verified"] and res["crc_device"] == zlib.crc32(image)
        if not no_reset:
            ok &= res["reset"] and chip.mode == "app"
        if not ok:
            diff = next((i for i in range(len(image)) if chip.flash[i] != image[i]), None)
            desc += f" | flash differs at {diff}, mode {chip.mode}, verified {res['verified']}"
    if not expect_ok and not res["erased"]:
        if bytes(chip.flash) != before or DESTRUCTIVE & set(chip.cmds):
            ok = False
            desc += " | touched the flash without saying so"
    if extra:
        e = extra(res, chip)
        if e:
            ok = False
            desc += f" | {e}"
    check(f"flash: {name}", ok, desc)
    return res


def untouched(res, chip):
    """Refused before the dongle was touched at all."""
    if chip.line_changes or chip.cmds or res["erased"]:
        return f"dongle touched: lines {chip.line_changes}, cmds {chip.cmds[:5]}"
    return None


def no_erase(res, chip):
    if DESTRUCTIVE & set(chip.cmds) or res["erased"]:
        return f"erased/wrote: {sorted(DESTRUCTIVE & set(chip.cmds))}"
    return None


# ---------------------------------------------------------------- Intel HEX

def to_hex(mem: dict[int, bytes], *, rec=16, mode="linear", crlf=False, lower=False, start=True, eof=True,
           reverse=False) -> str:
    """mem: {address: bytes}. mode: linear (04) or segment (02)."""
    nl = "\r\n" if crlf else "\n"
    recs = []
    for base, data in sorted(mem.items()):
        for i in range(0, len(data), rec):
            a, chunk = base + i, data[i:i + rec]
            # don't let a record cross a 64 KB boundary
            if (a & 0xFFFF) + len(chunk) > 0x10000:
                k = 0x10000 - (a & 0xFFFF)
                recs.append((a, chunk[:k]))
                recs.append((a + k, chunk[k:]))
            else:
                recs.append((a, chunk))
    if reverse:
        recs.reverse()

    def line(t, off, d):
        b = bytes([len(d), off >> 8, off & 0xFF, t]) + d
        s = ":" + (b + bytes([(-sum(b)) & 0xFF])).hex().upper()
        return s.lower() if lower else s

    out, cur = [], None
    for a, d in recs:
        if mode == "linear":
            hi, off = a >> 16, a & 0xFFFF
            if hi != cur:
                out.append(line(4, 0, struct.pack(">H", hi)))
                cur = hi
        else:
            seg = (a >> 4) & 0xF000
            off = a - (seg << 4)
            if seg != cur:
                out.append(line(2, 0, struct.pack(">H", seg)))
                cur = seg
        out.append(line(0, off, d))
    if start:
        out.append(line(3 if mode == "segment" else 5, 0, bytes.fromhex("00002001")))
    if eof:
        out.append(":00000001FF")
    return nl.join(out) + nl


def parse_hex_py(text: str) -> dict[int, int]:
    """Independent reference parser (no checks beyond checksums)."""
    mem, base = {}, 0
    for ln in text.splitlines():
        ln = ln.strip()
        if not ln:
            continue
        b = bytes.fromhex(ln[1:])
        assert sum(b) & 0xFF == 0
        n, off, t, d = b[0], (b[1] << 8) | b[2], b[3], b[4:4 + b[0]]
        if t == 0:
            for i in range(n):
                mem[base + off + i] = d[i]
        elif t == 2:
            base = ((d[0] << 8) | d[1]) << 4
        elif t == 4:
            base = ((d[0] << 8) | d[1]) << 16
        elif t == 1:
            break
    return mem


def ihex_run(text, maxchunk=0, seed=0, refuse_at=None):
    got = {}
    order = []

    def on_data(addr, data):
        if refuse_at is not None and addr == refuse_at:
            return 1
        order.append(addr)
        for i, v in enumerate(data):
            got[addr + i] = v
        return 0

    res = run(["ihex", maxchunk, seed], text.encode(), None, on_data=on_data)
    res["mem"] = got
    res["order"] = order
    return res


def hcheck(name, text, *, err_has=None, want=None, maxchunks=(0, 1, 7, 64, 1000), extra=None):
    for mc in maxchunks:
        for seed in ((0,) if mc in (0, 1) else (1, 2)):
            res = ihex_run(text, mc, seed)
            if err_has is None:
                ref = want if want is not None else parse_hex_py(text)
                ok = res["err"] == "" and res["mem"] == ref and res["eof"] == 1
                if ref:
                    ok &= res["min"] == min(ref) and res["end"] == max(ref) + 1 and res["total"] == len(ref)
                desc = f"err {res['err']!r}, total {res['total']} vs {len(ref)}"
            else:
                ok = err_has.lower() in res["err"].lower()
                desc = res["err"]
            if extra and ok:
                e = extra(res)
                if e:
                    ok = False
                    desc += f" | {e}"
            if not ok:
                check(f"ihex: {name} (pieces <= {mc or 'all'}, seed {seed})", False, desc)
                return res
    check(f"ihex: {name}", True)
    return res


def hex_line(t, off, d, bad=False):
    b = bytes([len(d), off >> 8, off & 0xFF, t]) + d
    return ":" + (b + bytes([((-sum(b)) + (1 if bad else 0)) & 0xFF])).hex().upper()


def ihex_tests():
    rnd = random.Random(5)
    mem = {0x0: bytes(rnd.getrandbits(8) for _ in range(5000)), 0x1FFF8: bytes(rnd.getrandbits(8) for _ in range(40)),
           0x57FA8: bytes(rnd.getrandbits(8) for _ in range(88))}
    lin = to_hex(mem)
    hcheck("linear addressing (04), LF", lin)
    hcheck("segment addressing (02), CRLF", to_hex(mem, mode="segment", crlf=True))
    hcheck("lowercase, 32-byte records, CR only", to_hex(mem, rec=32, lower=True).replace("\n", "\r"))
    hcheck("255-byte records", to_hex(mem, rec=255))
    hcheck("records in descending order (not overlapping) are fine", to_hex(mem, reverse=True))
    hcheck("blank lines, spaces and trailing whitespace", "\n\n  " + lin.replace("\n", "  \r\n\r\n") + "\n \t\n")
    hcheck("no newline after the EOF record", lin.rstrip("\n"))
    hcheck("start address records (03 and 05) ignored", hex_line(3, 0, bytes(4)) + "\n" + lin)
    hcheck("zero-length data record", hex_line(0, 0x10, b"") + "\n" + lin)
    lines = lin.splitlines()
    bad = list(lines)
    bad[5] = bad[5][:-2] + "%02X" % ((int(bad[5][-2:], 16) + 1) & 0xFF)
    hcheck("bad checksum (line number reported)", "\n".join(bad) + "\n", err_has="line 6: checksum mismatch")
    hcheck("bad checksum, CRLF line counting", "\r\n".join(bad) + "\r\n", err_has="line 6: checksum")
    hcheck("no EOF record", "\n".join(lines[:-1]) + "\n", err_has="without an end-of-file")
    hcheck("cut mid-record", lin[:lin.index(":", len(lin) // 2) + 9], err_has="cut short (4 bytes")
    hcheck("data after EOF", lin + hex_line(0, 0, b"\x01"), err_has="after the end-of-file")
    hcheck("garbage after EOF", lin + "junk\n", err_has="after the end-of-file")
    hcheck("not hex at all", "hello world\n", err_has="line 1: unexpected 'h'")
    hcheck("bad digit", ":10000000ZZ\n", err_has="not a hex digit")
    hcheck("odd number of digits", ":0100000001F\n", err_has="odd number")
    hcheck("record longer than its count", hex_line(0, 0, b"\x01\x02") + "00\n", err_has="extra characters")
    hcheck("two records on one line", hex_line(0, 0, b"\x01") + hex_line(0, 1, b"\x02") + "\n", err_has="two records")
    hcheck("unknown record type", hex_line(6, 0, b"") + "\n", err_has="unknown record type")
    hcheck("EOF record with data", hex_line(1, 0, b"\x00") + "\n", err_has="end-of-file record with")
    hcheck("bad 04 length", hex_line(4, 0, b"\x00") + "\n", err_has="extended linear")
    hcheck("bad 02 length", hex_line(2, 0, b"\x00\x00\x00") + "\n", err_has="extended segment")
    hcheck("bad 05 length", hex_line(5, 0, b"\x00") + "\n", err_has="start address")
    hcheck("overlapping records", hex_line(0, 0x100, bytes(16)) + "\n" + hex_line(0, 0x108, bytes(16)) + "\n:00000001FF\n",
           err_has="line 2: data at 0x00000108 overlaps")
    hcheck("same record twice", hex_line(0, 0x100, b"\x01" * 4) + "\n" + hex_line(0, 0x100, b"\x01" * 4) + "\n:00000001FF\n",
           err_has="overlaps")
    hcheck("overlap across segment/linear aliases",
           hex_line(2, 0, b"\x10\x00") + "\n" + hex_line(0, 0x0000, bytes(8)) + "\n" + hex_line(4, 0, b"\x00\x01") + "\n" +
           hex_line(0, 0x0004, bytes(8)) + "\n:00000001FF\n", err_has="overlaps")
    hcheck("record crossing a 64 KB boundary", hex_line(0, 0xFFF8, bytes(16)) + "\n:00000001FF\n", err_has="64 KB")
    # Ends exactly at 4 GB: hi would wrap to 0 and a 32-bit caller's addr + n check with it (heap write before the buffer)
    hcheck("record ending exactly at 4 GB refused",
           hex_line(4, 0, b"\xFF\xFF") + "\n" + hex_line(0, 0xFF01, bytes(0xFF)) + "\n:00000001FF\n", err_has="past 4 GB")
    hcheck("record ending just below 4 GB is fine",
           hex_line(4, 0, b"\xFF\xFF") + "\n" + hex_line(0, 0xFF00, bytes(0xFF)) + "\n:00000001FF\n")
    frag = "".join(hex_line(0, i * 32, b"\xAA") + "\n" for i in range(33)) + ":00000001FF\n"
    hcheck("more than 32 separate ranges refused", frag, err_has="too fragmented")
    adj = "".join(hex_line(0, i * 4, b"\xAA" * 4) + "\n" for i in range(64)[::-1]) + ":00000001FF\n"
    hcheck("descending adjacent records merge (no fragmentation)", adj)
    res = ihex_run(lin, 13, 4, refuse_at=0x1FFF8)
    check("ihex: the caller can stop the parse", "rejected by the caller at 0x0001fff8" in res["err"], res["err"])
    res = ihex_run(lin, 1, 0)
    check("ihex: a byte at a time", res["err"] == "" and res["chunks"] == len(lin), f"{res['err']} {res['chunks']}")
    hcheck("empty file", "", err_has="without an end-of-file")


# ---------------------------------------------------------------- the real thing

def fetch_real():
    cache = Path(tempfile.gettempdir()) / "tiflash_test" / Path(REAL_URL).name
    try:
        if not cache.exists():
            cache.parent.mkdir(parents=True, exist_ok=True)
            req = urllib.request.Request(REAL_URL, headers={"User-Agent": "tiflash-host-test"})
            with urllib.request.urlopen(req, timeout=30) as r:
                data = r.read(5_000_000)
            cache.write_bytes(data)
        z = zipfile.ZipFile(io.BytesIO(cache.read_bytes()))
        name = next(n for n in z.namelist() if n.lower().endswith(".hex"))
        return name, z.read(name).decode("ascii")
    except Exception as e:  # no network, moved release, ...
        print(f"skip real Z-Stack image: {e}")
        return None, None


def real_tests():
    name, text = fetch_real()
    if text is None:
        return
    res = ihex_run(text, 997, 7)
    flat = bytearray(b"\xff" * res["end"])
    for a, v in res["mem"].items():
        flat[a] = v
    ref = parse_hex_py(text)
    check(f"real: {name} parsed by ihex_core ({res['records']} records, {res['total']} bytes, end 0x{res['end']:x})",
          res["err"] == "" and res["mem"] == ref and res["end"] == K352, res["err"])
    image = bytes(flat)
    c = run(["check", 0], image, None)
    check("real: image check passes (bsl-config 0x00057FD8:0xC50FFEC5 per Koenkk's index.json)",
          c["check"] == "" and c["bl_config"] == 0xC5FE0FC5 and image[0x57FD8:0x57FDC] == bytes.fromhex("C50FFEC5")
          and c["bl_pin"] == 15 and c["bl_level"] == 0, c)
    flash("real Z-Stack coordinator onto a SONOFF-style board (backdoor DIO15)", image, Chip(),
          extra=lambda r, d: None if r["skipped"] > 600 else f"skipped {r['skipped']}")


# ---------------------------------------------------------------- main

def main():
    build()
    img = make_image()
    n_chunks = (K352 + 247) // 248

    # ---- the image checker
    c = run(["check", 0], img, None)
    check("check: good image", c["check"] == "" and c["crc"] == zlib.crc32(img) and c["bl_pin"] == 15 and c["ff"] > 1000, c)
    check("check: P7 image", run(["check", 0], make_image(K704), None)["check"] == "")
    for nm, im, cf, want in [
        ("backdoor disabled", make_image(bl=bl_config(backdoor=0x00)), 0, "disables the bootloader backdoor"),
        ("bootloader disabled", make_image(bl=bl_config(boot=0x00)), 0, "disables the ROM bootloader"),
        ("blank CCFG", make_image(blank_ccfg=True), 0, "disables the ROM bootloader"),
        ("backdoor pin out of range", make_image(bl=bl_config(pin=0xFF)), 0, "doesn't exist"),
        ("bank erase disabled", make_image(erase_conf=0xFFFFFFFE), 0, "disables bank erase"),
        ("chip erase disabled", make_image(erase_conf=0xFFFFFEFF), 0, "disables chip erase"),
        ("sector 43 write-protected", make_image(prot=[0xFFFFFFFF, 0xFFFFF7FF, 0xFFFFFFFF, 0xFFFFFFFF]), 0, "sector 43"),
        ("wrong size (256 KB)", img[:0x40000], 0, "whole flash"),
        ("wrong size (a hex's end, 0x57FF0)", img[:0x57FF0], 0, "whole flash"),
        ("352 KB image for a 704 KB chip", img, K704, "wrong firmware"),
        ("unsupported chip size", img, 0x80000, "Unsupported"),
        ("no vector table (image offset)", b"\xff" * 0x100 + img[0x100:], 0, "vector table"),
    ]:
        r = run(["check", cf], im, None)
        check(f"check: {nm} refused", want.lower() in r["check"].lower(), r["check"])
    check("check: protect bits beyond the chip's sectors ignored",
          run(["check", 0], make_image(prot=[0xFFFFFFFF, 0x00000FFF, 0, 0]), None)["check"] == "")

    # ---- flashing
    good = lambda r, d: None if (r["written"] + r["skipped"] == n_chunks and r["skipped"] == r["ff"] and r["retries"] == 0
                                 and r["passes"] == 1 and r["erase_how"] == "bank erase" and d.entries == 1
                                 and r["device_flash"] == K352 and r["wafer"] == 0xBB41) else \
        {k: r[k] for k in ("written", "skipped", "ff", "retries", "passes", "erase_how", "device_flash", "wafer")}
    flash("SONOFF pattern on a SONOFF board", img, Chip(), extra=good)
    flash("default DTR/RTS on a direct-wired board", img, Chip(board="direct"), entry="default", extra=good)
    flash("default, inverted lines, on a board with RTS->BSL", img, Chip(board="direct", board_invert=True),
          entry="default", invert=True)
    flash("default, active high, on an active-high backdoor", make_image(bl=bl_config(level=1)),
          Chip(board="direct", wired_level=1, flash=make_image(seed=98, bl=bl_config(level=1))), entry="default",
          active_high=True)
    flash("none: already in the bootloader (button)", img, Chip(board=None, start="bsl"), entry="none")
    flash("blank chip enters the bootloader on its own", img, Chip(board=None, start="bsl",
                                                                   flash=b"\xff" * K352), entry="none")
    flash("P7: 704 KB image on a 704 KB chip", make_image(K704), Chip(flash_size=K704), extra=lambda r, d: None if
          r["device_flash"] == K704 else r["device_flash"])
    flash("all-0xFF chunks skipped, flash still exact", img, Chip(flash=make_image(seed=50)),
          extra=lambda r, d: None if r["skipped"] == r["ff"] > 1000 and r["downloads"] >= 6 and
          r["bytes"] == (r["written"] - 1) * 248 + K352 % 248  # the last, short chunk holds the CCFG
          else (r["skipped"], r["ff"], r["downloads"], r["bytes"]))
    flash("sector erase on request", img, Chip(), sector=True,
          extra=lambda r, d: None if r["erase_how"] == "sector erase" and d.cmds.count(0x26) == 44 else r["erase_how"])
    # the CCFG sector first: cut off after any erase, the chip boots a blank CCFG (stays in the ROM bootloader)
    flash("sector erase starts with the CCFG sector", img, Chip(), sector=True,
          extra=lambda r, d: None if d.erase_order[:1] == [K352 // SECTOR - 1] and sorted(d.erase_order) == list(range(K352 // SECTOR))
          else d.erase_order[:3])
    flash("chip's CCFG disables bank erase: sector erase instead", img,
          Chip(flash=make_image(seed=97, erase_conf=0xFFFFFFFE)),
          extra=lambda r, d: None if r["erase_how"] == "sector erase" and 0x2C not in d.cmds else r["erase_how"])
    flash("left in the bootloader on request", img, Chip(), no_reset=True,
          extra=lambda r, d: None if not r["reset"] and d.mode == "bsl" else d.mode)
    flash("re-run while still synced from before (0x55s taken as a packet, padded, NACKed, PING)", img,
          Chip(start="synced", board=None), entry="none", extra=lambda r, d: None if d.entries == 0 else d.entries)

    # wrong entry patterns
    flash("SONOFF pattern on a direct-wired board: no bootloader", img, Chip(board="direct"), entry="sonoff",
          expect_ok=False, err_has="No answer from the stick's bootloader", extra=no_erase)
    flash("default pattern on a SONOFF board: no bootloader", img, Chip(), entry="default", expect_ok=False,
          err_has="No answer", extra=lambda r, d: no_erase(r, d) or (None if d.entries == 0 else "entered"))
    flash("default, wrong invert: no bootloader", img, Chip(board="direct"), entry="default", invert=True,
          expect_ok=False, err_has="No answer", extra=no_erase)
    flash("default, active high on an active-low backdoor: no bootloader", img, Chip(board="direct"), entry="default",
          active_high=True, expect_ok=False, err_has="No answer", extra=no_erase)
    flash("none, but the dongle is running its firmware", img, Chip(), entry="none", expect_ok=False,
          err_has="BOOT button", extra=no_erase)
    flash("backdoor pin not wired to the circuit (image for another board on the chip)", img,
          Chip(flash=make_image(seed=96, bl=bl_config(pin=13))), expect_ok=False, err_has="No answer", extra=no_erase)

    # images refused before the dongle is touched
    flash("image with the backdoor disabled: refused before anything", make_image(bl=bl_config(backdoor=0)), Chip(),
          expect_ok=False, err_has="backdoor", extra=untouched)
    flash("image with bank erase disabled: refused before anything", make_image(erase_conf=0xFFFFFFFE), Chip(),
          expect_ok=False, err_has="bank erase", extra=untouched)
    flash("wrong-size image: refused before anything", img[:0x50000], Chip(), expect_ok=False, err_has="whole flash",
          extra=untouched)
    # refused after asking the chip, before erasing
    flash("704 KB image on a 352 KB chip: refused before erasing", make_image(K704), Chip(), expect_ok=False,
          err_has="352 KB of flash but the image is for 704 KB", extra=no_erase)
    flash("352 KB image on a 704 KB chip: refused before erasing", img, Chip(flash_size=K704), expect_ok=False,
          err_has="704 KB of flash", extra=no_erase)
    flash("image for another board's backdoor pin: refused", make_image(bl=bl_config(pin=13)), Chip(), expect_ok=False,
          err_has="DIO13 active low but this stick's is DIO15", extra=no_erase)
    flash("... unless allowed", make_image(bl=bl_config(pin=13)), Chip(), allow_pin=True)
    flash("chip without MEMORY_READ: refused (size unknown)", img, Chip(mem_read_ok=False), expect_ok=False,
          err_has="flash size", extra=no_erase)
    flash("... unless the size check is skipped", img, Chip(mem_read_ok=False), skip_size=True)
    flash("CC26x0 chip refused", img, Chip(wafer=0xB99A), expect_ok=False, err_has="CC26x0", extra=no_erase)

    # sync / baud
    flash("chip never syncs", img, Chip(never_sync=True), expect_ok=False, err_has="No answer",
          extra=lambda r, d: no_erase(r, d) or (None if d.entries == 2 else f"entries {d.entries}"))
    flash("can't autobaud at 460800: falls back to 115200 after a fresh entry", img, Chip(max_baud=200000),
          extra=lambda r, d: None if r["baud"] == 115200 and d.entries == 2 else (r["baud"], d.entries))
    flash("asked for 115200 straight away", img, Chip(max_baud=200000), baud=115200,
          extra=lambda r, d: None if r["baud"] == 115200 and d.entries == 1 else r["baud"])
    flash("no fallback when told so", img, Chip(max_baud=200000), baud=460800, fallback=460800, expect_ok=False,
          err_has="No answer")

    # retries
    flash("every 9th SEND_DATA NACKed", img, Chip(nack_every=9),
          extra=lambda r, d: None if r["retries"] >= d.sd_seen // 9 - 1 > 20 else r["retries"])
    flash("every 11th SEND_DATA corrupted on the wire", img, Chip(corrupt_every=11),
          extra=lambda r, d: None if r["retries"] >= d.sd_seen // 11 - 1 > 15 else r["retries"])
    flash("a byte lost from every 25th SEND_DATA", img, Chip(drop_every=25),
          extra=lambda r, d: None if r["retries"] >= d.sd_seen // 25 - 1 > 5 and r["downloads"] >= 6 + r["retries"]
          else (r["retries"], r["downloads"]))
    flash("ACK lost after every 30th SEND_DATA (written twice)", img, Chip(lose_ack_every=30),
          extra=lambda r, d: None if r["retries"] >= d.sd_seen // 30 - 1 > 5 else r["retries"])
    flash("every SEND_DATA NACKed: gives up", img, Chip(nack_every=1), expect_ok=False, err_has="kept failing at 0x00000",
          extra=lambda r, d: None if r["erased"] else "not marked erased")
    flash("USB stick pulled mid-write", img, Chip(gone_at_chunk=200), expect_ok=False, err_has="went away",
          extra=lambda r, d: None if r["erased"] else "not marked erased")

    # verify
    flash("CRC wrong once: erased and written again", img, Chip(crc_lie_once=True),
          extra=lambda r, d: None if r["passes"] == 2 and d.cmds.count(0x2C) == 2 else (r["passes"], d.cmds.count(0x2C)))
    stuck = next(i for i in range(0x100, len(img)) if img[i] & 0x01)
    flash("stuck flash bit: CRC mismatch detected", img, Chip(stuck=(stuck, 0xFE)), expect_ok=False,
          err_has="doesn't match the image after writing it twice",
          extra=lambda r, d: None if not r["verified"] and not r["reset"] and r["passes"] == 2 else r)

    flash("progress reported", img, Chip(),
          extra=lambda r, d: None if ("write", K352, K352) in r["progress"] and ("verify", K352, K352) in r["progress"]
          else r["progress"][-4:])

    # ---- Intel HEX
    ihex_tests()
    real_tests()

    print(f"\n{len(FAILS)} failed" if FAILS else "\nall passed")
    sys.exit(1 if FAILS else 0)


if __name__ == "__main__":
    main()
