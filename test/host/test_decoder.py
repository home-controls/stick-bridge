"""Feeds the firmware's own decoder (built for the host by harness.c) a scripted Zigbee2MQTT <-> coordinator
conversation, encoded here independently (ASH per UG101 / zigbee-herdsman: CRC-CCITT, LFSR randomisation,
byte stuffing), and checks every counter, device row and event against what the script did.

Each trace is fed byte by byte and in random chunks with several seeds: the results must match exactly.

  python test_decoder.py            (from Windows; builds and runs the harness in WSL with ASan + UBSan)
"""
import json
import random
import struct
import subprocess
import sys
from pathlib import Path

HERE = Path(__file__).resolve().parent
MAIN = HERE.parent.parent / "main"
FLAG, ESC, XON, XOFF, SUB, CAN = 0x7E, 0x7D, 0x11, 0x13, 0x18, 0x1A
RESERVED = {FLAG, ESC, XON, XOFF, SUB, CAN}
HOST, NCP = 0, 1
EDGES_US = [1000, 2000, 5000, 10000, 20000, 50000, 100000, 200000, 500000, 1000000, 2000000]


def crc16(data: bytes) -> int:
    crc = 0xFFFF
    for b in data:
        crc ^= b << 8
        for _ in range(8):
            crc = ((crc << 1) ^ 0x1021) & 0xFFFF if crc & 0x8000 else (crc << 1) & 0xFFFF
    return crc


def lfsr(n: int) -> list[int]:
    out, seed = [], 0x42
    for _ in range(n):
        out.append(seed)
        seed = (seed >> 1) ^ 0xB8 if seed & 1 else seed >> 1
    return out


def randomise(data: bytes) -> bytes:
    return bytes(b ^ r for b, r in zip(data, lfsr(len(data))))


def stuff(raw: bytes) -> bytes:
    out = bytearray()
    for b in raw:
        if b in RESERVED:
            out += bytes([ESC, b ^ 0x20])
        else:
            out.append(b)
    return bytes(out)


def frame(control: int, data: bytes = b"", *, bad_crc=False) -> bytes:
    body = bytes([control]) + data
    c = crc16(body) ^ (0x0001 if bad_crc else 0)
    return stuff(body + struct.pack(">H", c)) + bytes([FLAG])


def data_frame(frm: int, ack: int, ezsp: bytes, retx=False, **kw) -> bytes:
    return frame((frm << 4) | (0x08 if retx else 0) | ack, randomise(ezsp), **kw)


def ezsp_ext(seq: int, fc_lo: int, frame_id: int, params: bytes = b"") -> bytes:
    return bytes([seq, fc_lo, 0x01]) + struct.pack("<H", frame_id) + params


def bucket(us: int) -> int:
    b = 0
    while b < len(EDGES_US) and us >= EDGES_US[b]:
        b += 1
    return b


class Script:
    """Builds the conversation and, as it goes, what the decoder should count."""

    def __init__(self) -> None:
        self.recs: list[tuple[int, int, bytes]] = []
        self.t = 100_000
        self.host_frm = self.ncp_frm = 0
        self.seq = 0
        self.e = {"host": dict.fromkeys(("bytes", "frames", "data", "ack", "nak", "rst", "rstack", "error", "retx",
                                         "crc_err", "cancel", "substitute", "oversize"), 0)}
        self.e["ncp"] = dict(self.e["host"])
        for k in ("ezsp_cmds", "ezsp_responses", "ezsp_callbacks", "zb_rx", "zb_unicast", "zb_broadcast",
                  "zb_multicast", "zb_sent_ok", "zb_sent_fail", "route_errors", "net_status", "joins", "leaves",
                  "id_conflicts", "rtt_n", "rtt_sum_us", "rtt_max_us", "ack_n", "ack_sum_us", "ack_max_us"):
            self.e[k] = 0
        self.e["rtt_hist"] = [0] * 12
        self.devices: dict[int, dict] = {}
        self.events: list[str] = []
        self.pending_ack: dict[int, int] = {}
        self.burst = False

    def emit(self, d: int, data: bytes, dt_us: int = 0) -> None:
        self.t += dt_us
        if self.burst and dt_us == 0 and self.recs and self.recs[-1][0] == d:
            # back-to-back frames arrive in one read, as they do from the dongle's USB stream
            pd, pt, pdata = self.recs[-1]
            self.recs[-1] = (pd, pt, pdata + data)
        else:
            self.recs.append((d, self.t, data))
        self.e["host" if d == HOST else "ncp"]["bytes"] += len(data)

    def count(self, d: int, kind: str, n=1) -> None:
        self.e["host" if d == HOST else "ncp"][kind] += n

    def dev(self, short: int) -> dict:
        return self.devices.setdefault(short, {"rx": 0, "tx_ok": 0, "tx_fail": 0, "route_err": 0, "lqi": 0, "rssi": 0,
                                               "ieee": "0000000000000000", "last_fail_status": 0})

    # ------------------------------------------------------------- host side

    def host_cmd(self, ezsp: bytes, dt_us: int, frame_id: int, retx_after: int | None = None) -> int:
        frm = self.host_frm
        self.host_frm = (self.host_frm + 1) % 8
        self.emit(HOST, data_frame(frm, self.ncp_frm, ezsp), dt_us)
        self.count(HOST, "frames")
        self.count(HOST, "data")
        self.e["ezsp_cmds"] += 1
        if frame_id == 0x0034:
            self.e["zb_unicast"] += 1
        elif frame_id == 0x0036:
            self.e["zb_broadcast"] += 1
        self.pending_ack[frm] = self.t
        sent_at = self.t
        if retx_after is not None:  # the same frame again, retransmit flag set
            self.emit(HOST, data_frame(frm, self.ncp_frm, ezsp, retx=True), retx_after)
            self.count(HOST, "frames")
            self.count(HOST, "data")
            self.count(HOST, "retx")
        return sent_at

    def host_ack(self, dt_us=300) -> None:
        self.emit(HOST, frame(0x80 | self.ncp_frm), dt_us)
        self.count(HOST, "frames")
        self.count(HOST, "ack")

    # ------------------------------------------------------------- ncp side

    def _acked(self) -> None:
        """An NCP frame carries ackNum = host_frm: every host frame before it is acknowledged now."""
        for frm, at in list(self.pending_ack.items()):
            lat = self.t - at
            self.e["ack_n"] += 1
            self.e["ack_sum_us"] += lat
            self.e["ack_max_us"] = max(self.e["ack_max_us"], lat)
            del self.pending_ack[frm]

    def ncp_ack(self, dt_us: int) -> None:
        self.emit(NCP, frame(0x80 | self.host_frm), dt_us)
        self.count(NCP, "frames")
        self.count(NCP, "ack")
        self._acked()

    def ncp_data(self, ezsp: bytes, dt_us: int, **kw) -> None:
        frm = self.ncp_frm
        self.ncp_frm = (self.ncp_frm + 1) % 8
        self.emit(NCP, data_frame(frm, self.host_frm, ezsp, **kw), dt_us)
        if kw.get("bad_crc"):
            self.count(NCP, "crc_err")
            return
        self.count(NCP, "frames")
        self.count(NCP, "data")
        self._acked()

    def response(self, seq: int, frame_id: int, sent_at: int, dt_us: int, params=b"\x00") -> None:
        self.ncp_data(ezsp_ext(seq, 0x80, frame_id, params), dt_us)
        rtt = self.t - sent_at
        self.e["ezsp_responses"] += 1
        self.e["rtt_n"] += 1
        self.e["rtt_sum_us"] += rtt
        self.e["rtt_max_us"] = max(self.e["rtt_max_us"], rtt)
        self.e["rtt_hist"][bucket(rtt)] += 1

    def callback(self, frame_id: int, params: bytes, dt_us=2000) -> None:
        self.ncp_data(ezsp_ext(self.next_seq(), 0x90, frame_id, params), dt_us)
        self.e["ezsp_callbacks"] += 1

    def next_seq(self) -> int:
        self.seq = (self.seq + 1) % 256
        return self.seq


APS = struct.pack("<HHBBHHB", 0x0104, 0x0006, 1, 1, 0x0140, 0, 7)  # 11 bytes


def build(rnd: random.Random) -> Script:
    s = Script()
    # reset: CANCEL then RST from Zigbee2MQTT, RSTACK (ASH v2, reset code 11 = software) back
    s.emit(HOST, bytes([CAN]) + frame(0xC0), 0)
    s.count(HOST, "frames")
    s.count(HOST, "rst")
    s.events.append("Zigbee2MQTT reset the coordinator link (ASH RST)")
    s.emit(NCP, frame(0xC1, bytes([0x02, 0x0B])), 50_000)
    s.count(NCP, "frames")
    s.count(NCP, "rstack")
    s.events.append("Coordinator reset acknowledged (ASH v2, reset code 11)")

    # version: legacy request, then the extended one
    at = s.host_cmd(bytes([0, 0x00, 0x00, 13]), 50_000, 0x0000)
    s.ncp_ack(10_000)
    s.ncp_data(bytes([0, 0x80, 0x00, 19, 2]) + struct.pack("<H", 0x9110), 5_000)
    s.e["ezsp_responses"] += 1
    rtt = s.t - at
    s.e["rtt_n"] += 1
    s.e["rtt_sum_us"] += rtt
    s.e["rtt_max_us"] = max(s.e["rtt_max_us"], rtt)
    s.e["rtt_hist"][bucket(rtt)] += 1
    s.events.append("Coordinator: EZSP v19, stack 9.1.1.0")
    s.host_ack()
    at = s.host_cmd(ezsp_ext(1, 0x00, 0x0000, bytes([13])), 80_000, 0x0000)
    s.response(1, 0x0000, at, 20_000, bytes([19, 2]) + struct.pack("<H", 0x9110))  # same version: no new event
    s.host_ack()
    s.seq = 1

    # commands with known response times (and some retransmits)
    lat_ms = [0.6, 1.5, 3, 7, 12, 35, 70, 150, 300, 700, 1500, 2500, 9, 11, 14]
    for i, ms in enumerate(lat_ms):
        seq = s.next_seq()
        fid = 0x0034 if i % 3 else 0x0036
        at = s.host_cmd(ezsp_ext(seq, 0x00, fid, bytes(8)), 200_000, fid, retx_after=400 if i % 5 == 4 else None)
        resp_us = int(ms * 1000)
        if resp_us > 1500:
            s.ncp_ack(1200)
            resp_us -= 1200
        s.response(seq, fid, at, max(resp_us - (400 if i % 5 == 4 else 0), 50))
        s.host_ack()

    # callbacks: messages heard from five devices
    devs = {0x072F: (0x00124B0012345601, 160, -61), 0x3BE9: (0x00124B0012345602, 90, -79),
            0x1A2B: (0x00124B0012345603, 40, -88), 0xC177: (0x00124B0000005604, 230, -50),
            0x0D9A: (0x0, 120, -70)}
    for rnd_round in range(4):
        for short, (ieee, lqi, rssi) in devs.items():
            lq = max(0, min(255, lqi + rnd.randint(-5, 5)))
            rs = max(-100, min(-20, rssi + rnd.randint(-3, 3)))
            # message contents chosen so the randomised bytes hit every reserved value (stuffing coverage)
            contents = bytes([0x18, 0x01, 0x0A, 0x00, 0x00, 0x10, rnd_round])
            params = (bytes([0]) + APS + struct.pack("<HQBBBbI", short, ieee, 0xFF, 0xFF, lq, rs, 0)
                      + bytes([len(contents)]) + contents)
            s.callback(0x0045, params)
            d = s.dev(short)
            d["rx"] += 1
            d["lqi"], d["rssi"] = lq, rs
            if ieee:
                d["ieee"] = f"{ieee:016x}"
            s.e["zb_rx"] += 1
    # a callback whose parameters, once randomised, are all reserved bytes
    base = 5 + 1 + 11 + 18
    lf = lfsr(base + 12)
    tricky = bytes(sorted(RESERVED)[i % 6] ^ lf[base + i] for i in range(12))
    params = bytes([0]) + APS + struct.pack("<HQBBBbI", 0x072F, 0x00124B0012345601, 0xFF, 0xFF, 150, -60, 0) + bytes([12]) + tricky
    s.callback(0x0045, params)
    d = s.dev(0x072F)
    d["rx"] += 1
    d["lqi"], d["rssi"] = 150, -60
    s.e["zb_rx"] += 1

    # deliveries: three ok to 0x072f, two failed to 0x3be9, one broadcast ok
    def sent(status: int, typ: int, dest: int) -> None:
        s.callback(0x003F, struct.pack("<IBH", status, typ, dest) + APS + struct.pack("<H", 1))
        if status == 0:
            s.e["zb_sent_ok"] += 1
        else:
            s.e["zb_sent_fail"] += 1
        if typ == 0:
            d = s.dev(dest)
            if status == 0:
                d["tx_ok"] += 1
            else:
                d["tx_fail"] += 1
                d["last_fail_status"] = status
    for _ in range(3):
        sent(0, 0, 0x072F)
    for _ in range(2):
        sent(0x0C02, 0, 0x3BE9)
    sent(0, 6, 0xFFFD)

    # route error, network status, stack up, join, leave, address conflict: one burst from the coordinator
    s.burst = True
    s.callback(0x0080, struct.pack("<IH", 0x0C26, 0x1A2B))
    s.e["route_errors"] += 1
    s.dev(0x1A2B)["route_err"] += 1
    s.events.append("Route error to 0x1a2b: NO_ROUTE")
    s.callback(0x00C4, struct.pack("<BH", 0x0B, 0x0D9A), 0)
    s.e["net_status"] += 1
    s.dev(0x0D9A)["route_err"] += 1
    s.callback(0x0019, struct.pack("<I", 0x0015), 0)  # EZSP v19: sl_status_t NETWORK_UP
    s.events.append("Zigbee network is up")
    s.callback(0x0024, struct.pack("<HQBBH", 0x9999, 0x00158D0001ABCDEF, 1, 0, 0x0000), 0)
    s.e["joins"] += 1
    s.events.append("Device 0x9999 (0x00158d0001abcdef) joined via 0x0000")
    s.callback(0x0024, struct.pack("<HQBBH", 0x9999, 0x00158D0001ABCDEF, 2, 0, 0x0000), 0)
    s.e["leaves"] += 1
    s.events.append("Device 0x9999 (0x00158d0001abcdef) left the network")
    s.callback(0x007C, struct.pack("<H", 0x1234), 0)
    s.e["id_conflicts"] += 1
    s.events.append("Address conflict for 0x1234")
    s.burst = False

    # line trouble: a bad CRC, a NAK, a SUBSTITUTE, an ASH ERROR, a cancelled frame, junk too long for a
    # frame, stray XON/XOFF between frames
    s.ncp_data(ezsp_ext(s.next_seq(), 0x90, 0x0005), 3000, bad_crc=True)
    s.emit(HOST, frame(0xA0 | s.ncp_frm), 1000)
    s.count(HOST, "frames")
    s.count(HOST, "nak")
    good = frame(0x80 | s.host_frm)
    s.emit(NCP, good[:1] + bytes([SUB]) + good[1:], 1000)
    s.count(NCP, "substitute")
    s.count(NCP, "crc_err")
    s.emit(NCP, frame(0xC2, bytes([0x02, 0x51])), 1000)
    s.count(NCP, "frames")
    s.count(NCP, "error")
    s.e["last_error_code"] = 0x51
    s.events.append("Coordinator sent ASH ERROR (code 0x51)")
    s.emit(NCP, bytes([0x25, 0x42, 0x21, CAN]), 1000)
    s.count(NCP, "cancel")
    s.emit(NCP, bytes(rnd.randrange(0x20, 0x70) for _ in range(400)) + bytes([FLAG]), 1000)
    s.count(NCP, "oversize")
    s.emit(HOST, bytes([XON, XOFF]), 1000)
    s.host_ack()
    return s


def write_trace(s: Script, path: Path) -> None:
    with path.open("wb") as f:
        for d, t, data in s.recs:
            f.write(struct.pack("<BqH", d, t, len(data)) + data)


def wsl(p: Path) -> str:
    if sys.platform != "win32":  # Linux (CI): run_all.py puts linux/wsl in front, paths stay as they are
        return p.resolve().as_posix()
    return "/mnt/" + p.drive[0].lower() + p.as_posix()[2:]


def build_harness() -> str:
    exe = "/tmp/zbb_harness"
    srcs = " ".join(wsl(MAIN / f) for f in ("ezsp_decode.c", "stats.c"))
    cmd = (f"gcc -std=gnu11 -O1 -g -Wall -Wextra -Werror -Wno-unused-parameter -fsanitize=address,undefined "
           f"-fno-sanitize-recover=all -I{wsl(HERE / 'stubs')} -I{wsl(MAIN)} {wsl(HERE / 'harness.c')} {srcs} -o {exe}")
    r = subprocess.run(["wsl", "-d", "Ubuntu-24.04", "--", "bash", "-c", cmd], capture_output=True, text=True)
    if r.returncode:
        print(r.stdout, r.stderr)
        sys.exit("harness build failed")
    return exe


def run(exe: str, trace: Path, seed: int) -> dict:
    r = subprocess.run(["wsl", "-d", "Ubuntu-24.04", "--", exe, wsl(trace), str(seed)], capture_output=True, text=True)
    if r.returncode:
        print(r.stdout[-2000:], r.stderr[-4000:])
        sys.exit(f"harness failed (seed {seed})")
    return json.loads(r.stdout)


def compare(s: Script, got: dict, label: str) -> list[str]:
    bad = []
    e = s.e

    def eq(name, want, have):
        if want != have:
            bad.append(f"{label}: {name}: want {want}, got {have}")
    eq("lock_violations", 0, got["lock_violations"])
    for side in ("host", "ncp"):
        for k, v in e[side].items():
            eq(f"{side}.{k}", v, got[side][k])
    for k in ("ezsp_cmds", "ezsp_responses", "ezsp_callbacks", "zb_rx", "zb_unicast", "zb_broadcast", "zb_multicast",
              "zb_sent_ok", "zb_sent_fail", "route_errors", "net_status", "joins", "leaves", "id_conflicts",
              "rtt_n", "rtt_sum_us", "rtt_max_us", "rtt_hist", "ack_n", "ack_sum_us", "ack_max_us"):
        eq(k, e[k], got[k])
    eq("ezsp_version", 19, got["ezsp_version"])
    eq("stack_version", 0x9110, got["stack_version"])
    eq("last_reset_code", 11, got["last_reset_code"])
    eq("last_error_code", 0x51, got["last_error_code"])
    eq("network_up", 1, got["network_up"])
    have = {d["short"]: d for d in got["devices"]}
    eq("device set", sorted(s.devices), sorted(have))
    for short, want in s.devices.items():
        h = have.get(short, {})
        for k, v in want.items():
            eq(f"device 0x{short:04x}.{k}", v, h.get(k))
    eq("events", s.events, [x["text"] for x in got["events"]])
    eq("history rx", e["zb_rx"], got["hist"]["rx"])
    eq("history ok", e["zb_sent_ok"], got["hist"]["ok"])
    eq("history fail", e["zb_sent_fail"], got["hist"]["fail"])
    eq("history bytes in", e["ncp"]["bytes"], got["hist"]["in"])
    eq("history bytes out", e["host"]["bytes"], got["hist"]["out"])
    return bad


def fuzz_trace(path: Path, seed: int) -> None:
    """Random bytes, valid frames cut short, reserved bytes in bursts: the decoder must just survive."""
    rnd = random.Random(seed)
    s = Script()
    for i in range(6000):
        d = rnd.choice((HOST, NCP))
        kind = rnd.random()
        if kind < 0.3:
            data = bytes(rnd.randrange(256) for _ in range(rnd.randrange(1, 600)))
        elif kind < 0.5:
            data = bytes(rnd.choice(sorted(RESERVED)) for _ in range(rnd.randrange(1, 40)))
        else:
            ez = bytes(rnd.randrange(256) for _ in range(rnd.randrange(0, 240)))
            fr = data_frame(rnd.randrange(8), rnd.randrange(8), ez, retx=rnd.random() < 0.2, bad_crc=rnd.random() < 0.1)
            data = fr[: rnd.randrange(1, len(fr) + 1)] if rnd.random() < 0.3 else fr
        s.emit(d, data, rnd.randrange(0, 50_000))
    write_trace(s, path)


def many_devices_trace(path: Path, n: int) -> Script:
    s = Script()
    for k in range(n):
        short = 0x1000 + k
        params = bytes([0]) + APS + struct.pack("<HQBBBbI", short, 0x00124B0000000000 + k, 0xFF, 0xFF, 100, -70, 0) + b"\x00"
        s.callback(0x0045, params, 20_000)
    write_trace(s, path)
    return s


# stackStatusHandler: EZSP v14+ sends a 4-byte sl_status_t (NETWORK_UP 0x15, DOWN 0x16), older versions one
# EmberStatus byte (0x90, 0x91). Before the version is seen, the length decides.
# (EZSP version or None, params, want network_up_known, want network_up, want last event or None)
STACK_CASES = [
    (19, struct.pack("<I", 0x0015), 1, 1, "Zigbee network is up"),
    (19, struct.pack("<I", 0x0016), 1, 0, "Zigbee network is DOWN"),
    (19, struct.pack("<I", 0x0090), 0, 0, "Stack status 0x0090"),
    (14, struct.pack("<I", 0x0015), 1, 1, "Zigbee network is up"),
    (13, bytes([0x90]), 1, 1, "Zigbee network is up"),
    (13, bytes([0x91]), 1, 0, "Zigbee network is DOWN"),
    (13, bytes([0x15]), 0, 0, "Stack status 0x0015"),
    (None, struct.pack("<I", 0x0015), 1, 1, "Zigbee network is up"),
    (None, bytes([0x90]), 1, 1, "Zigbee network is up"),
    (19, bytes([0x15]), 0, 0, None),  # cut short: ignored
]


def stack_trace(path: Path, version, params: bytes) -> None:
    s = Script()
    if version is not None:
        s.ncp_data(bytes([0, 0x80, 0x00, version, 2]) + struct.pack("<H", 0x9120), 5_000)
    s.callback(0x0019, params)
    write_trace(s, path)


def check_stack(exe: str, trace: Path) -> list[str]:
    bad = []
    for version, params, known, up, event in STACK_CASES:
        stack_trace(trace, version, params)
        got = run(exe, trace, 99999)
        texts = [x["text"] for x in got["events"] if not x["text"].startswith("Coordinator:")]
        have = (got["network_up_known"], got["network_up"], texts[-1] if texts else None)
        if have != (known, up, event):
            bad.append(f"stack status v{version} {params.hex()}: want {(known, up, event)}, got {have}")
    return bad


def main() -> None:
    exe = build_harness()
    trace = HERE / "trace.bin"
    problems = []
    for script_seed in (1, 2, 3):
        s = build(random.Random(script_seed))
        write_trace(s, trace)
        for feed_seed in (0, 1, 7, 42, 1234, 99999):  # 99999: each read handed over whole
            got = run(exe, trace, feed_seed)
            problems += compare(s, got, f"script {script_seed} feed {feed_seed}")
    for seed in (11, 12, 13):  # fuzz: no crash, no sanitizer report, no double lock
        fuzz_trace(trace, seed)
        for feed_seed in (3, 99999):
            got = run(exe, trace, feed_seed)
            if got["lock_violations"]:
                problems.append(f"fuzz {seed}: lock taken twice")
    problems += check_stack(exe, trace)
    s = many_devices_trace(trace, 250)  # more devices than the table holds (DEV_MAX 192)
    got = run(exe, trace, 99999)
    if len(got["devices"]) != 192 or got["zb_rx"] != 250:
        problems.append(f"many devices: {len(got['devices'])} rows, zb_rx {got['zb_rx']}")
    kept = {d["short"] for d in got["devices"]}
    if not {0x1000 + k for k in range(250 - 50, 250)} <= kept:
        problems.append("many devices: the most recently heard ones were not all kept")
    trace.unlink(missing_ok=True)
    if problems:
        print("\n".join(problems[:60]))
        print(f"\n{len(problems)} mismatches")
        sys.exit(1)
    print("decoder: all counters, devices, events and history match (3 scripts x 6 feeds), fuzz survived, "
          f"{len(STACK_CASES)} stack status cases, device table overflow OK (ASan+UBSan clean)")


if __name__ == "__main__":
    main()
