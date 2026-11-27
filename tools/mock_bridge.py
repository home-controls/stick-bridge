"""A stand-in bridge API for working on the dashboard without the board or its password: always signed in,
data from Zigbee Studio's adapter simulator (zigbee-studio repo, zigbee_studio/dev/adapter_sim.py).

  python tools/mock_bridge.py [port=18300] [path to adapter_sim.py's folder]
  then in web/:  BRIDGE=http://127.0.0.1:18300 npm run dev

MOCK_RADIOS=4 shows a hub on J3 with radios that come and go (2.0): radio 1 Zigbee2MQTT, radio 2 a ZBDongle-P for ZHA,
radio 3 a Thread stick not set up yet, radio 4 free. The default is one stick on J3. Test-only endpoints drive the radios:
  POST /api/mock/plug {"radio": n, "product": "thread"|"mg24"|"dongle-p"|"zwave"}   a stick appears (opening, then ready)
  POST /api/mock/unplug {"radio": n}   set up: the radio waits ("missing"); else it disappears
  POST /api/mock/client {"radio": n}   a client connects (the radio is set up)
  POST /api/mock/error {"radio": n, "error": "..."}   set or clear a radio's error
"""
import hashlib
import os
import sys
from pathlib import Path

from aiohttp import web

SIM_DIR = Path(sys.argv[2]) if len(sys.argv) > 2 else Path(__file__).resolve().parents[2] / "zigbee-studio" / "zigbee_studio" / "dev"
sys.path.insert(0, str(SIM_DIR))
import adapter_sim  # noqa: E402

sim = adapter_sim.AdapterSim()
sim.cfg.update({"update_url": "https://github.com/home-controls/stick-bridge/releases/latest/download/manifest.json", "update_auto": True,
                "tx_chunk": 64, "tx_gap_us": 300, "xoff_honor": True, "xoff_max_ms": 20, "hw_flow": False,
                "fw_repo": "Nerivec/silabs-firmware-builder", "fw_prerelease": True, "role": "zigbee"})
# Example LAN addresses (the README's): the bridge at .50, Home Assistant (where Zigbee2MQTT connects from) at .10
HA_IP = "192.168.1.10"
sim.cfg.update({"ip": "192.168.1.50", "allow": HA_IP})
for e in sim.events:
    e["text"] = e["text"].replace("172.30.33.5", HA_IP)
# Recordings carry real devices' IEEE addresses: keep each maker's prefix (the first 3 bytes), invent the rest. The
# hash is keyed with a secret made fresh each run and never stored: an unkeyed hash of 5 unknown bytes could be
# searched back to the real address.
_IEEE_KEY = os.urandom(16)
for d in sim.devs:
    if d.get("ieee", "").startswith("0x") and len(d["ieee"]) == 18:
        d["ieee"] = d["ieee"][:8] + hashlib.sha256(_IEEE_KEY + d["ieee"].encode()).hexdigest()[:10]

DONGLE_ID = {"usb_chip": "CP210x", "model": "sonoff_dongle-pmg24", "model_name": "SONOFF Dongle Plus MG24", "radio": "EFR32MG24",
             "family": "Silicon Labs", "protocol": "EmberZNet (EZSP)", "protocol_detected": True, "z2m_adapter": "ember",
             "model_baud": 115200, "rtscts_wired": False, "bootloader_entry": "DTR/RTS reset", "fw_board": "sonoff_dongle-pmg24",
             "flashable": True, "note": "Software flow control only (no RTS/CTS)"}


# ---------------------------------------------------------------- radios (2.0: automatic, up to 4)
RADIO_MAX = 4
RADIOS = int(os.environ.get("MOCK_RADIOS", "1") or 1)
MULTI = RADIOS > 1
ROLE_NAME = {"zigbee": "Zigbee", "zha": "ZHA", "thread": "Thread", "zwave": "Z-Wave"}
# Made-up serial numbers (never real ones)
STICK_KINDS = {
    "mg24": {"usb_id": "10c4:ea60", "manufacturer": "SONOFF", "product": "SONOFF Dongle Plus MG24", "usb_chip": "CP210x",
             "model": "sonoff_dongle-pmg24", "model_name": "SONOFF Dongle Plus MG24", "family": "Silicon Labs",
             "protocol": "EmberZNet (EZSP)", "protocol_detected": True, "z2m_adapter": "ember", "fw_running": "", "baud": 115200},
    "thread": {"usb_id": "10c4:ea60", "manufacturer": "SONOFF", "product": "SONOFF Dongle Plus MG24", "usb_chip": "CP210x",
               "model": "sonoff_dongle-pmg24", "model_name": "SONOFF Dongle Plus MG24", "family": "Silicon Labs",
               "protocol": "OpenThread RCP (Spinel)", "protocol_detected": True, "z2m_adapter": "",
               "fw_running": "SL-OPENTHREAD/2.6.1.0_GitHub-1fceb225b; EFR32; Nov 20 2026 10:11:12", "baud": 460800, "role": "thread"},
    "dongle-p": {"usb_id": "10c4:ea60", "manufacturer": "SONOFF", "product": "SONOFF Zigbee 3.0 USB Dongle Plus", "usb_chip": "CP210x",
                 "model": "sonoff_zbdongle-p", "model_name": "SONOFF ZBDongle-P", "family": "Texas Instruments",
                 "protocol": "Z-Stack (ZNP)", "protocol_detected": True, "z2m_adapter": "zstack",
                 "fw_running": "Z-Stack 3.x.0 build 20240710", "baud": 115200, "role": "zha"},
    "zwave": {"usb_id": "1a86:55d4", "manufacturer": "Zooz", "product": "ZST39 800 Long Range", "usb_chip": "CH9102",
              "model": "zooz_zst39", "model_name": "Zooz ZST39 LR", "family": "Silicon Labs", "protocol": "Z-Wave Serial API",
              "protocol_detected": True, "z2m_adapter": "", "fw_running": "", "baud": 115200, "role": "zwave"},
}
SERIAL_N = [0]


def make_stick(kind, hub_port):
    k = dict(STICK_KINDS.get(kind, STICK_KINDS["thread"]))
    SERIAL_N[0] += 1
    k["serial"] = f"{SERIAL_N[0]:032d}"
    k["hub_port"] = hub_port
    return k


sim.cfg.update({"usb_fs_only": True})
# radio 0's port/allow/baud/role are the flat config fields; LINKS[0] holds its name, gen and set-up flag
LINKS = [{"index": i, "name": "", "tcp_port": 6638 + i, "allow": HA_IP, "baud": 115200, "role": "zigbee", "gen": 1, "set_up": False}
         for i in range(RADIO_MAX)]
STICKS = {i: None for i in range(RADIO_MAX)}
SAVED = {i: None for i in range(RADIO_MAX)}
CLIENT = {i: False for i in range(RADIO_MAX)}  # radio 0's client is the simulator's
OPEN_T = {i: 0.0 for i in range(RADIO_MAX)}
ERRORS = {i: "" for i in range(RADIO_MAX)}

STICKS[0] = make_stick("mg24", 1 if MULTI else 0)
LINKS[0]["set_up"] = True
if MULTI:
    STICKS[1] = make_stick("dongle-p", 2)
    LINKS[1].update(role="zha", set_up=True)
    CLIENT[1] = True
    STICKS[2] = make_stick("thread", 3)
    LINKS[2].update(role="thread", baud=460800)
for i, k in STICKS.items():
    if k:
        SAVED[i] = {"usb_id": k["usb_id"], "serial": k["serial"], "hub_port": k["hub_port"]}


def link(i):
    """A radio's link with radio 0's flat fields folded in."""
    l = dict(LINKS[i])
    if i == 0:
        l.update(tcp_port=sim.cfg["tcp_port"], allow=sim.cfg["allow"], baud=sim.cfg["baud"], role=sim.cfg.get("role", "zigbee"))
    return l


def shown(i):
    return i == 0 or bool(STICKS[i]) or LINKS[i]["set_up"]


def flashing(i):
    return DFW["flash"].get("state") == "running" and DFW.get("radio", 0) == i


def radio_state(i):
    if not STICKS[i]:
        return "missing" if LINKS[i]["set_up"] else "no_stick"
    if flashing(i):
        return "flashing"
    if time.time() - OPEN_T[i] < 3:
        return "opening"
    return "ready"


def client_on(i, s=None):
    if not STICKS[i] or radio_state(i) != "ready":
        return False
    return bool(s["client"]["connected"]) if i == 0 and s else CLIENT[i]


def bump(i):
    LINKS[i]["gen"] += 1


def radios_status(s):
    out = []
    for i in range(RADIO_MAX):
        if not shown(i):
            continue
        l, k = link(i), STICKS[i]
        connected = client_on(i, s)
        rate_in = (212, 96, 0, 0)[i] if connected else 0
        rate_out = (140, 61, 0, 0)[i] if connected else 0
        out.append({
            "index": i, "state": radio_state(i), "set_up": l["set_up"], "error": ERRORS[i], "gen": l["gen"],
            "name": l["name"] or ROLE_NAME[l["role"]], "role": l["role"], "tcp_port": l["tcp_port"], "allowed": l["allow"], "baud": l["baud"],
            "stick": {"present": bool(k), "open": bool(k) and radio_state(i) in ("ready", "flashing"), "since": 120,
                      **({kk: vv for kk, vv in k.items() if kk not in ("baud", "role")} if k else {})},
            "saved_stick": SAVED[i],
            "client": {"connected": connected, "ip": HA_IP if connected else "", "port": 40000 + i, "since": 300},
            "counters": {"bytes_in": 812345 // (i + 1) if k else 0, "bytes_out": 501234 // (i + 1) if k else 0, "bps_in": rate_in, "bps_out": rate_out,
                         "clients_accepted": 3 if l["set_up"] else 0, "clients_refused": 1 if i == 1 else 0,
                         "client_drops": 1 if l["set_up"] else 0, "dropped_no_client": 0 if l["set_up"] else 2048, "dropped_overflow": 0,
                         "tx_errors": 0, "attach": 1 if SAVED[i] else 0, "detach": 0 if k or not SAVED[i] else 1},
        })
    return out


def usb_state():
    devs = []
    if MULTI:
        devs.append({"addr": 1, "hub_port": 0, "hub": True, "speed": "full", "usb_id": "05e3:0610", "product": "USB2.1 Hub", "radio": None})
    for i, k in STICKS.items():
        if k:
            devs.append({"addr": 2 + i, "hub_port": k["hub_port"], "hub": False, "speed": "full", "usb_id": k["usb_id"], "product": k["product"], "radio": i})
    return {"fs_only": sim.cfg["usb_fs_only"], "fs_forced": sim.cfg["usb_fs_only"], "reforces": 0, "refused": 0,
            "radio_max": RADIO_MAX, "devices": devs}


def radio_index(x):
    i = x.get("radio") if isinstance(x, dict) else None
    return i if isinstance(i, int) and 0 <= i < RADIO_MAX else None


async def radio_forget(r):
    i = radio_index(await r.json())
    if i is None:
        return web.json_response({"error": f"radio is 0-{RADIO_MAX - 1}"}, status=400)
    if STICKS[i]:
        return web.json_response({"error": "Its stick is plugged in: unplug it first, then forget the radio"}, status=409)
    LINKS[i]["set_up"] = False
    SAVED[i] = None
    CLIENT[i] = False
    CATS.pop(i, None)
    bump(i)
    sim.event(0, f"Radio {i + 1} forgotten from the dashboard")
    sim.events[-1]["radio"] = i
    return web.json_response({"ok": True})


def free_port():
    used = {link(i)["tcp_port"] for i in range(RADIO_MAX) if shown(i)}
    p = 6638
    while p in used:
        p += 1
    return p


async def mock_plug(r):
    j = await r.json()
    i = radio_index(j)
    if i is None:
        return web.json_response({"error": f"radio is 0-{RADIO_MAX - 1}"}, status=400)
    kind = (j.get("product") or "thread").lower()
    k = make_stick(kind, i + 1)
    was = shown(i)
    if not LINKS[i]["set_up"] and i > 0:  # a fresh radio: role and baud from the stick, a free port, radio 1's allowed host
        LINKS[i].update(role=k.get("role", "thread"), baud=k["baud"], allow=sim.cfg["allow"], name="")
        if not was:
            LINKS[i]["tcp_port"] = free_port()
    STICKS[i] = k
    SAVED[i] = SAVED[i] or {"usb_id": k["usb_id"], "serial": k["serial"], "hub_port": k["hub_port"]}
    OPEN_T[i] = time.time()
    bump(i)
    sim.event(0, f"Radio {i + 1}: stick {k['model_name']} plugged into hub port {k['hub_port']}")
    sim.events[-1]["radio"] = i
    return web.json_response({"ok": True})


async def mock_unplug(r):
    i = radio_index(await r.json())
    if i is None:
        return web.json_response({"error": f"radio is 0-{RADIO_MAX - 1}"}, status=400)
    STICKS[i] = None
    CLIENT[i] = False
    if not LINKS[i]["set_up"] and i > 0:
        SAVED[i] = None
    bump(i)
    sim.event(1 if LINKS[i]["set_up"] else 0, f"Radio {i + 1}: stick unplugged" + (", waiting for it" if LINKS[i]["set_up"] else ", radio removed"))
    sim.events[-1]["radio"] = i
    return web.json_response({"ok": True})


async def mock_client(r):
    i = radio_index(await r.json())
    if i is None:
        return web.json_response({"error": f"radio is 0-{RADIO_MAX - 1}"}, status=400)
    CLIENT[i] = True
    if not LINKS[i]["set_up"]:
        LINKS[i]["set_up"] = True
        bump(i)
    sim.event(0, f"Radio {i + 1}: client connected from {HA_IP}")
    sim.events[-1]["radio"] = i
    return web.json_response({"ok": True})


async def mock_error(r):
    j = await r.json()
    i = radio_index(j)
    if i is None:
        return web.json_response({"error": f"radio is 0-{RADIO_MAX - 1}"}, status=400)
    ERRORS[i] = j.get("error", "")
    return web.json_response({"ok": True})


def config_view():
    c = dict(sim.cfg)
    c["radios"] = [{**{k: v for k, v in link(i).items()}, "shown": shown(i)} for i in range(RADIO_MAX)]
    return c


def status_with_flow():
    s = sim.status()
    c = sim.cfg
    s["counters"]["flow"] = {"xon_rx": 7, "xoff_rx": 7, "xoff_now": False, "xoff_pauses": 5, "xoff_timeouts": 0,
                             "xoff_pause_avg_ms": 1.8, "xoff_pause_max_ms": 4.2, "writes": 5210, "chunks": 5388,
                             "paced": 241, "pace_avg_us": 1180, "pace_max_us": 1702, "write_max_ms": 6.4,
                             "write_errors": 0, "queue_hwm": 312, "chunk": c["tx_chunk"], "gap_us": c["tx_gap_us"],
                             "xoff_honor": c["xoff_honor"], "xoff_max_ms": c["xoff_max_ms"]}
    s["dongle"].update(DONGLE_ID)
    s["client"]["ip"] = HA_IP
    s["sys"].update(psram_total=33554432, psram_free=33100000, psram_min=33000000)
    # this dongle has no CTS wire (DONGLE_ID's note), so CTS reads off in every sample
    s["counters"]["line"] = {"known": True, "cts_now": False, "hw_flow": c["hw_flow"], "samples": 41230, "cts_off": 41230,
                             "cts_drops": 0, "status_polls": 4123, "hold_cts": 0, "hold_xon": 0, "err_framing": 0,
                             "err_hw_overrun": 2, "err_queue_overrun": 0, "err_parity": 0, "err_break": 0,
                             "in_queue_max": 118, "out_queue_max": 64, "poll_fail": 0, "hw_auto_off": 0}
    s["radios"] = radios_status(s)
    s["dongle"]["fw_running"] = "EmberZNet 9.1.1.0 (EZSP v18)"
    return s
LOG = "".join(f"I ({i * 137}) web: mock line {i} with °C and a long tail to see wrapping on a narrow screen\n" for i in range(40))


async def state(_r):
    return web.json_response({"setup_required": False, "authed": True, "locked": False, "api_token_set": True,
                              "api": "zbbridge/1", "version": "1.0.7", "hostname": "zigbee-eth-bridge"})


def answer(fn):
    async def h(_r):
        return web.json_response(fn())
    return h


def event_radio(e):
    if "radio" not in e:
        t = e["text"]
        e["radio"] = -1 if any(w in t for w in ("Booted", "Ethernet", "firmware", "SD card", "hub")) else 0
    return e


if MULTI:
    for lv, txt, rad in ((0, "USB hub on J3 (4 ports, full speed)", -1),
                         (0, "Radio 2: stick SONOFF ZBDongle-P on hub port 2, Z-Stack (ZNP) detected, role ZHA", 1),
                         (0, f"Radio 2: client connected from {HA_IP}", 1),
                         (1, "Radio 2: client dropped (no data for 60 s), reconnected", 1),
                         (0, "Radio 3: stick SONOFF Dongle Plus MG24 on hub port 3, OpenThread RCP (Spinel) detected, role Thread", 2)):
        sim.event(lv, txt)
        sim.events[-1]["radio"] = rad


async def events(r):
    since = int(r.query.get("since", "0") or 0)
    return web.json_response({"events": [event_radio(e) for e in sim.events if e["id"] > since], "uptime": sim.up()})


async def log(_r):
    return web.json_response({"next": len(LOG), "text": LOG})


async def ok(_r):
    return web.json_response({"ok": True})


NETWORK_KEYS = ("hostname", "mode", "ip", "mask", "gw", "dns")


async def config_post(r):
    body = await r.json()
    before = config_view()
    radios = body.pop("radios", None)
    if radios is not None:
        # check everything first: a stale gen or a bad value changes nothing
        new = {i: link(i) for i in range(RADIO_MAX)}
        for x in radios:
            i = x.get("index")
            if not isinstance(i, int) or not 0 <= i < RADIO_MAX:
                return web.json_response({"error": f"radios: each entry needs an index 0-{RADIO_MAX - 1}"}, status=400)
            if x.get("gen") != LINKS[i]["gen"]:
                return web.json_response({"error": f"Radio {i + 1} changed meanwhile (a stick was plugged in or out): reload the page"}, status=409)
            if "role" in x and x["role"] not in ROLE_NAME:
                return web.json_response({"error": "radios: role is zigbee, zha, thread or zwave"}, status=400)
            if i == 0 and x.get("role") == "zha":
                return web.json_response({"error": "Radio 1 is the radio the dashboard decodes: use another radio for ZHA"}, status=400)
            cur = new[i]
            if flashing(i) and (x.get("tcp_port", cur["tcp_port"]) != cur["tcp_port"] or x.get("baud", cur["baud"]) != cur["baud"]):
                return web.json_response({"error": f"Radio {i + 1}'s stick is being flashed: change its port or baud rate afterwards"}, status=409)
            cur.update({k: v for k, v in x.items() if k in ("name", "tcp_port", "allow", "baud", "role")})
        ports = [(i, new[i]["tcp_port"]) for i in range(RADIO_MAX) if shown(i)]
        for n, (i, port) in enumerate(ports):
            for j, p in ports[:n]:
                if p == port:
                    return web.json_response({"error": f"Radio {i + 1}: TCP port {port} is already used by radio {j + 1}"}, status=400)
        for x in radios:
            i = x["index"]
            l = new[i]
            if i == 0:
                sim.cfg.update(tcp_port=l["tcp_port"], allow=l["allow"], baud=l["baud"], role=l["role"])
            LINKS[i].update({k: l[k] for k in ("name", "tcp_port", "allow", "baud", "role")})
            bump(i)
    sim.cfg.update({k: v for k, v in body.items() if k in sim.cfg})
    after = config_view()
    reboot = any(before.get(k) != after.get(k) for k in NETWORK_KEYS) or before["usb_fs_only"] != after["usb_fs_only"]
    return web.json_response({**after, "reboot_required": reboot})


UPD = {"state": "available", "current": "1.0.9", "latest": "1.0.10", "notes": "Dashboard layout fixes.\nUpdates from git with MD5 and SHA-256 checks.",
       "date": "2026-11-21", "size": 766000, "done": 0, "md5": "0" * 32, "sha256": "f" * 64, "checked_ago": 42, "error": "",
       "source": sim.cfg.get("update_url", ""), "auto": True}


async def update_get(_r):
    if UPD["state"] == "downloading":
        UPD["done"] = min(UPD["size"], UPD["done"] + 90000)
        if UPD["done"] >= UPD["size"]:
            UPD["state"] = "available"  # the mock never really reboots
            UPD["done"] = 0
    return web.json_response(UPD)


async def update_install(_r):
    UPD["state"] = "downloading"
    return web.json_response({"ok": True})


import hashlib  # noqa: E402
import time  # noqa: E402

DFW = {"max_size": 2093056, "baud": 115200, "busy": False, "dongle": True, "z2m_connected": True, "radio": 0,
       "current": "EmberZNet 9.1.1.0 (EZSP v18)", "image": None, "flash": {"state": "idle"}}
FLASH_T0 = [0.0]
B = "sonoff_dongle-pmg24_zigbee_ncp_2026.6.2_9.1.2_460800_"
CATS = {}    # radio -> its own last check
CAT_T = {}   # radio -> [check started, download started]
DL = {"state": "idle", "radio": 0, "name": "", "done": 0, "total": 0, "error": ""}  # one download at a time


def stick_kind(i):
    k = STICKS.get(i) or {}
    p = k.get("protocol", "")
    return "thread" if "Spinel" in p else "ti" if "Z-Stack" in p else "zwave" if "Z-Wave" in p else "ember"


def cat_for(i):
    if i not in CATS:
        kind = stick_kind(i)
        k = STICKS.get(i) or {}
        repo = "Koenkk/Z-Stack-firmware" if kind == "ti" else sim.cfg.get("fw_repo", "")
        CATS[i] = {"radio": i, "checking": False, "checked": 0, "checked_ago": -1, "error": "", "repo": repo, "release": "",
                   "prerelease": False, "board": k.get("model", ""), "model": k.get("model_name", ""),
                   "type": {"ti": "coordinator", "thread": "openthread_rcp"}.get(kind, "zigbee_ncp"),
                   "current": "9.1.1.0" if i == 0 else "", "builds": [],
                   "dongle_list": {"status": "12 sticks (2026-11-20)", "count": 12, "fetched_ago": 3600}}
        if kind == "zwave":
            CATS[i]["error"] = "No firmware updates through the bridge for this stick"
        CAT_T[i] = [0.0, 0.0]
    c = CATS[i]
    c["download"] = DL
    return c


def cat_tick(i):
    c = cat_for(i)
    if c["checking"] and time.time() - CAT_T[i][0] > 2:
        kind = stick_kind(i)
        if kind == "ti":
            builds = [{"name": "CC1352P2_CC2652P_launchpad_coordinator_20250321.zip", "version": "20250321", "sdk": "", "baud": 115200,
                       "flow": "none", "size": 180224, "sha256": "c" * 64, "usable": True, "recommended": True, "vs_current": "newer"}]
            rel = "Z-Stack_3.x.0_coordinator_20250321"
        elif kind == "thread":
            builds = [{"name": "sonoff_dongle-pmg24_openthread_rcp_2.6.1.0_GitHub-1fceb225b_460800_sw_flow.gbl", "version": "2.6.1.0",
                       "sdk": "2026.6.2", "baud": 460800, "flow": "sw_flow", "size": 210144, "sha256": "d" * 64, "usable": True,
                       "recommended": True, "vs_current": "same"}]
            rel = "v2026.6.2"
        else:
            builds = [{"name": B + "sw_flow.gbl", "version": "9.1.2", "sdk": "2026.6.2", "baud": 460800, "flow": "sw_flow", "size": 270232,
                       "sha256": "a" * 64, "usable": True, "recommended": True, "vs_current": "newer"},
                      {"name": B + "hw_flow.gbl", "version": "9.1.2", "sdk": "2026.6.2", "baud": 460800, "flow": "hw_flow", "size": 270232,
                       "sha256": "b" * 64, "usable": False, "recommended": False, "vs_current": "newer"}]
            rel = "v2026.6.2-pre1"
        c.update(checking=False, checked_ago=0, release=rel, prerelease=kind == "ember", builds=builds)
    if DL["state"] == "running" and DL["radio"] in CAT_T:
        t = time.time() - CAT_T[DL["radio"]][1]
        DL["done"] = min(DL["total"], int(DL["total"] * t / 4))
        if t >= 4:
            DL["state"] = "done"
            DFW["image"] = {"name": DL["name"], "size": DL["total"], "upload_size": DL["total"], "md5": "0" * 32, "sha256": "a" * 64,
                            "uploaded": int(time.time()), "signed": False, "encrypted": False, "fw_type": "zigbee_ncp",
                            "version": "9.1.2", "sdk": "2026.6.2", "image_baud": 460800}


def pick_radio(r):
    """?radio=n picks the radio the firmware pages work on (it stays picked)."""
    if "radio" in r.query:
        i = int(r.query["radio"])
        if not 0 <= i < RADIO_MAX:
            return web.json_response({"error": f"radio is 0-{RADIO_MAX - 1}"}, status=400)
        if DFW["busy"] and i != DFW["radio"]:
            return web.json_response({"error": "Another radio's stick is being flashed: wait for it to finish"}, status=409)
        DFW["radio"] = i
    i = DFW["radio"]
    DFW["dongle"] = bool(STICKS.get(i))
    DFW["baud"] = link(i)["baud"]
    DFW["z2m_connected"] = client_on(i, sim.status()) if i == 0 else client_on(i)
    # the running firmware shows once the stick has talked to its client
    DFW["current"] = None if not STICKS.get(i) or not LINKS[i]["set_up"] else \
        "EmberZNet 9.1.1.0 (EZSP v18)" if i == 0 else STICKS[i].get("fw_running") or None
    return None


def dfw_answer():
    i = DFW["radio"]
    cat_tick(i)
    return web.json_response({**DFW, "catalog": cat_for(i)})


async def fw_check(r):
    bad = pick_radio(r)
    if bad:
        return bad
    i = DFW["radio"]
    c = cat_for(i)
    if stick_kind(i) != "zwave":
        c["checking"] = True
        CAT_T[i][0] = time.time()
    return dfw_answer()


async def fw_download(r):
    bad = pick_radio(r)
    if bad:
        return bad
    i = DFW["radio"]
    name = (await r.json()).get("name", "")
    b = next((x for x in cat_for(i)["builds"] if x["name"] == name), None)
    if not b:
        return web.json_response({"error": "That file isn't in this radio's last check: check again"}, status=409)
    DL.update(state="running", radio=i, name=name, done=0, total=b["size"], error="")
    CAT_T[i][1] = time.time()
    return dfw_answer()


MODELS = [{"id": "sonoff_dongle-pmg24", "name": "SONOFF Dongle Plus MG24", "usb_id": "10c4:ea60", "usb_chip": "CP210x", "radio": "EFR32MG24",
           "family": "Silicon Labs", "protocol": "EmberZNet (EZSP)", "baud": 115200, "rtscts_wired": False, "bootloader_entry": "DTR/RTS reset",
           "flashable": True, "note": "Software flow control only (no RTS/CTS)"},
          {"id": "sonoff_zbdongle-p", "name": "SONOFF ZBDongle-P", "usb_id": "10c4:ea60", "usb_chip": "CP210x", "radio": "CC2652P",
           "family": "Texas Instruments", "protocol": "Z-Stack (ZNP)", "baud": 115200, "rtscts_wired": False,
           "bootloader_entry": "TI BSL (SONOFF gated DTR/RTS)", "flashable": False, "note": ""},
          {"id": "conbee_2", "name": "ConBee II", "usb_id": "1cf1:0030", "usb_chip": "CDC-ACM", "radio": "ATSAMR21",
           "family": "dresden elektronik", "protocol": "deCONZ", "baud": 115200, "rtscts_wired": False,
           "bootloader_entry": "deCONZ bootloader command", "flashable": False, "note": "deCONZ firmware (.GCF); flashing not supported here yet"}]


async def dongle_get(r):
    bad = pick_radio(r)
    if bad:
        return bad
    f = DFW["flash"]
    if f["state"] == "running":  # walk through the steps in about 25 s
        t = time.time() - FLASH_T0[0]
        f["seconds"] = int(t)
        size = DFW["image"]["upload_size"]
        if t < 2:
            f["step"] = "image"
        elif t < 5:
            f.update(step="bootloader")
        elif t < 20:
            f.update(step="upload", done=int(size * (t - 5) / 15), total=size)
        elif t < 22:
            f.update(step="start")
        elif t < 25:
            f.update(step="verify")
        else:
            fail = DFW["image"]["name"].startswith("fail")
            f.update(state="failed" if fail else "done", before="EmberZNet 9.1.1.0 (EZSP v18)",
                     after=None if fail else "EmberZNet 9.1.2.0 (EZSP v19)", dongle_left="bootloader" if fail else None,
                     error="The upload kept failing at 62% (no answer)" if fail else None)
            f = {k: v for k, v in f.items() if v is not None}
            DFW["flash"] = f
            DFW["busy"] = False
            DFW["z2m_connected"] = True
    return dfw_answer()


async def dongle_image(r):
    body = await r.read()
    name = r.query.get("name", "firmware.gbl")
    if body[:4] != b"\xeb\x17\xa6\x03":
        return web.json_response({"error": "Not a GBL firmware image (.gbl)"}, status=400)
    DFW["image"] = {"name": name, "size": len(body), "upload_size": len(body), "md5": hashlib.md5(body).hexdigest(),
                    "sha256": hashlib.sha256(body).hexdigest(), "uploaded": int(time.time()), "signed": False,
                    "encrypted": False, "fw_type": "zigbee_ncp", "version": "9.1.2.0", "sdk": "2025.6.2", "image_baud": 115200}
    if "router" in name:
        DFW["image"].update(fw_type="zigbee_router",
                            problem="This is zigbee_router firmware, not a Zigbee coordinator (zigbee_ncp): Zigbee2MQTT can't use it")
    return dfw_answer()


async def dongle_delete(_r):
    DFW["image"] = None
    return dfw_answer()


async def dongle_flash(r):
    bad = pick_radio(r)
    if bad:
        return bad
    j = await r.json()
    if not DFW["image"] or j.get("md5") != DFW["image"]["md5"]:
        return web.json_response({"error": "That isn't the stored image (MD5 differs): reload the page"}, status=409)
    FLASH_T0[0] = time.time()
    DFW["flash"] = {"state": "running", "step": "image", "done": 0, "total": 0, "seconds": 0}
    DFW["busy"] = True
    DFW["z2m_connected"] = False
    return dfw_answer()


# ---- stored log (flash + SD) ----
import time as _time
from datetime import datetime as _dt, timezone as _tz


def _stamp(ms):
    return _dt.fromtimestamp(ms / 1000, _tz.utc).strftime("%Y-%m-%dT%H:%M:%S.") + f"{int(ms % 1000):03d}Z"


def _stored():
    now = _time.time() * 1000
    out = []
    for b, (boot, start) in enumerate([(41, now - 3 * 86400e3), (42, now - 2 * 3600e3)]):
        out.append(f"{_stamp(start)} # Boot {boot}: firmware 1.0.{15 + b}, reset: {'software restart' if b else 'power on'}")
        for i in range(600 if b else 300):
            ms = 400 + i * 9000
            lv = "W" if i % 37 == 5 else "E" if i % 151 == 9 else "I"
            out.append(f"{_stamp(start + ms)} {lv} ({ms}) {'bridge' if i % 3 else 'web'}: mock stored line {i} of boot {boot}")
        if not b:
            out.append(f"{_stamp(start + 2.7e6)} # Boot 41: its last lines, kept in memory through the reset (not saved before it)")
            out.append(f"{_stamp(start + 2.7e6)} E (2700000) watchdog: mock panic line before the reset")
    out.append(f"boot+0.409s # Boot 43: firmware 1.0.16, reset: power on")
    out.append(f"boot+0.409s # Clock not set yet: times are seconds since boot until it is")
    out.append(f"boot+1.200s I (1200) net: link up")
    return "\n".join(out) + "\n"


SD = {"state": "ok"}


async def logs_info(_r):
    now = _time.time() * 1000
    sd = {"state": SD["state"], "detail": "SDHC/SDXC 31.9 GB" if SD["state"] == "ok" else "No card", "retry_in": 12}
    if SD["state"] == "ok":
        files = [{"name": _dt.fromtimestamp((now - d * 86400e3) / 1000, _tz.utc).strftime("%Y-%m-%d") + ".log", "size": 140000 + d * 731} for d in range(14)]
        sd.update(total=31.9e9, free=31.2e9, card="SD32G", khz=40000, written=52311, mounted_s=3600,
                  file_count=len(files), file_bytes=sum(f["size"] for f in files), files=files)
    return web.json_response({
        "flash": {"ok": True, "where": "upper 16 MB of the 32 MB flash chip", "chip_id": "EF4019", "chip_size": 33554432, "boot": 43,
                  "clock_wait": False, "offset": 16777216, "size": 16756720, "used": 431000, "oldest": now - 3 * 86400e3,
                  "write_errors": 0, "pending": 0},
        "sd": sd})


async def logs_text(r):
    if r.query.get("src") == "sd":
        return web.Response(text=f"(mock day file {r.query.get('name')})\n", content_type="text/plain")
    t = _stored()
    tail = int(r.query.get("tail", "0") or 0)
    if tail and len(t) > tail:
        t = t[-tail:]
        t = t[t.index("\n") + 1:]
    headers = {"Content-Disposition": 'attachment; filename="stick-bridge-log.txt"'} if r.query.get("download") == "1" else {}
    return web.Response(text=t, content_type="text/plain", headers=headers)


async def sd_format(r):
    if not (await r.json()).get("confirm") is True:
        return web.json_response({"error": 'Send {"confirm": true} to erase the card'}, status=400)
    if SD["state"] == "none":
        return web.json_response({"error": "No card in the slot"}, status=409)
    SD["state"] = "ok"
    sim.event(1, "SD card formatted from the dashboard: everything on it was erased")
    sim.events[-1]["radio"] = -1
    return web.json_response({"ok": True})


async def sd_toggle(_r):
    SD["state"] = "none" if SD["state"] == "ok" else "ok"
    return web.json_response(SD)


app = web.Application(client_max_size=4 * 1024 * 1024)
app.router.add_get("/api/dongle", dongle_get)
app.router.add_get("/api/dongles", answer(lambda: MODELS))
app.router.add_post("/api/dongle/firmware/check", fw_check)
app.router.add_post("/api/dongle/firmware/download", fw_download)
app.router.add_get("/api/logs", logs_info)
app.router.add_get("/api/logs/text", logs_text)
app.router.add_post("/api/logs/clear", ok)
app.router.add_post("/mock/sd", sd_toggle)
app.router.add_post("/api/sd/format", sd_format)
app.router.add_post("/api/mock/plug", mock_plug)
app.router.add_post("/api/mock/unplug", mock_unplug)
app.router.add_post("/api/mock/client", mock_client)
app.router.add_post("/api/mock/error", mock_error)
app.router.add_post("/api/dongle/image", dongle_image)
app.router.add_post("/api/dongle/image/delete", dongle_delete)
app.router.add_post("/api/dongle/flash", dongle_flash)
app.router.add_get("/api/update", update_get)
app.router.add_post("/api/update/check", ok)
app.router.add_post("/api/update/install", update_install)
app.router.add_get("/api/state", state)
app.router.add_get("/api/status", answer(status_with_flow))
def add_radios(h):
    """bytes/s from / to each radio's stick, aligned with t (radio 0's are in/out; hidden or idle radios zeros)."""
    import math
    rad = [{"in": h["in"], "out": h["out"]}]
    for i in range(1, RADIO_MAX):
        on = MULTI and STICKS[i] and CLIENT[i]
        rad.append({"in": [int(80 + 60 * math.sin(t / 97 + i) + (t * 7919 % 37)) if on else 0 for t in h["t"]],
                    "out": [int(50 + 35 * math.sin(t / 131 + i) + (t * 104729 % 23)) if on else 0 for t in h["t"]]})
    h["radios"] = rad
    return h


async def history(r):
    h = sim.history()
    if r.query.get("res") != "10":
        return web.json_response(add_radios(h))
    # 10-second points for the long ranges: fold the simulator's seconds, then make up older ones
    import random
    n = int(r.query.get("n", "8640"))
    end = (h["t"][-1] // 10) * 10 if h["t"] else 0
    rnd = random.Random(1)
    out = {k: [] for k in h}
    for i in range(min(n, 8640)):
        t = end - (min(n, 8640) - 1 - i) * 10
        if t <= 0:
            continue
        busy = 1.5 if (t // 3600) % 6 in (1, 2) else 1.0
        out["t"].append(t)
        out["in"].append(int(rnd.uniform(200, 900) * busy)); out["out"].append(int(rnd.uniform(40, 200) * busy))
        out["rx"].append(int(rnd.uniform(4, 14) * busy)); out["ok"].append(int(rnd.uniform(2, 8) * busy))
        out["fail"].append(1 if rnd.random() < 0.03 else 0); out["cmds"].append(int(rnd.uniform(5, 20)))
        out["rtt"].append(round(rnd.uniform(6, 11), 1)); out["rttmax"].append(round(rnd.uniform(12, 30), 1)); out["err"].append(0)
    return web.json_response(add_radios(out))


def tasks_state():
    """GET /api/tasks (2.0.1): plausible stacks; radio tasks in PSRAM."""
    ours = [("tcp_tx", 6144, 3900, "psram"), ("tcp_rx", 6144, 3500, "psram")]
    for i in range(1, 4):
        ours += [(f"tcp_tx{i}", 5120, 3400, "psram"), (f"tcp_rx{i}", 5120, 3200, "psram")]
    ours += [("dongle", 6144, 2900, "internal"), ("dongle_line", 3072, 2100, "psram"), ("usb_lib", 4096, 2600, "internal"),
             ("fwcat", 8192, 3100, "internal"), ("logstore", 8192, 4400, "internal"), ("updater", 12288, 6900, "internal"),
             ("watchdog", 4096, 2500, "internal")]
    system = [("httpd", 10240, 4200), ("tiT", 3072, 1100), ("emac_rx", 4096, 2400), ("USB-CDC", 6144, 3700),
              ("esp_timer", 3584, 2000), ("Tmr Svc", 2048, 1200), ("IDLE0", 1536, 900), ("IDLE1", 1536, 920),
              ("ipc0", 1024, 480), ("ipc1", 1024, 500)]
    tasks = [{"name": n, "stack": s, "min_free": f, "where": w} for n, s, f, w in ours]
    tasks += [{"name": n, "stack": s, "min_free": f, "where": "internal", "system": True} for n, s, f in system]
    heap = lambda free, mn, lg, tot: {"free": free, "min": mn, "largest": lg, "total": tot}
    return {"tasks": tasks, "memory": {"internal": heap(301000, 262000, 180224, 520000), "dma": heap(210000, 190000, 120000, 400000),
                                        "psram": heap(33100000, 33050000, 31457280, 33554432)}}


app.router.add_get("/api/tasks", answer(tasks_state))
app.router.add_get("/api/history", history)
app.router.add_get("/api/devices", answer(sim.devices))
app.router.add_get("/api/config", answer(config_view))
app.router.add_get("/api/usb", answer(usb_state))
app.router.add_post("/api/radio/forget", radio_forget)
app.router.add_post("/api/config", config_post)
app.router.add_get("/api/events", events)
app.router.add_get("/api/log", log)
for p in ("/api/reboot", "/api/stats/reset", "/api/logout"):
    app.router.add_post(p, ok)
web.run_app(app, host="127.0.0.1", port=int(sys.argv[1]) if len(sys.argv) > 1 else 18300)
