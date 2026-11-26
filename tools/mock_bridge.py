"""A stand-in bridge API for working on the dashboard without the board or its password: always signed in,
data from Zigbee Studio's adapter simulator (zigbee-studio repo, zigbee_studio/dev/adapter_sim.py).

  python tools/mock_bridge.py [port=18300] [path to adapter_sim.py's folder]
  then in web/:  BRIDGE=http://127.0.0.1:18300 npm run dev
"""
import hashlib
import sys
from pathlib import Path

from aiohttp import web

SIM_DIR = Path(sys.argv[2]) if len(sys.argv) > 2 else Path(__file__).resolve().parents[2] / "zigbee-studio" / "zigbee_studio" / "dev"
sys.path.insert(0, str(SIM_DIR))
import adapter_sim  # noqa: E402

sim = adapter_sim.AdapterSim()
sim.cfg.update({"update_url": "https://github.com/home-controls/zigbee-bridge/releases/latest/download/manifest.json", "update_auto": True,
                "tx_chunk": 64, "tx_gap_us": 300, "xoff_honor": True, "xoff_max_ms": 20, "hw_flow": False,
                "fw_repo": "Nerivec/silabs-firmware-builder", "fw_prerelease": True, "role": "zigbee"})
# Example LAN addresses (the README's): the bridge at .50, Home Assistant (where Zigbee2MQTT connects from) at .10
HA_IP = "192.168.1.10"
sim.cfg.update({"ip": "192.168.1.50", "allow": HA_IP})
for e in sim.events:
    e["text"] = e["text"].replace("172.30.33.5", HA_IP)
# Recordings carry real devices' IEEE addresses: keep each maker's prefix (the first 3 bytes), invent the rest
for d in sim.devs:
    if d.get("ieee", "").startswith("0x") and len(d["ieee"]) == 18:
        d["ieee"] = d["ieee"][:8] + hashlib.sha256(d["ieee"].encode()).hexdigest()[:10]

DONGLE_ID = {"usb_chip": "CP210x", "model": "sonoff_dongle-pmg24", "model_name": "SONOFF Dongle Plus MG24", "radio": "EFR32MG24",
             "family": "Silicon Labs", "protocol": "EmberZNet (EZSP)", "protocol_detected": True, "z2m_adapter": "ember",
             "model_baud": 115200, "rtscts_wired": False, "bootloader_entry": "DTR/RTS reset", "fw_board": "sonoff_dongle-pmg24",
             "flashable": True, "note": "Software flow control only (no RTS/CTS)"}


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
    return s
LOG = "".join(f"I ({i * 137}) web: mock line {i} with °C and a long tail to see wrapping on a narrow screen\n" for i in range(40))


async def state(_r):
    return web.json_response({"setup_required": False, "authed": True, "locked": False, "api_token_set": True,
                              "api": "zbbridge/1", "version": "1.0.7", "hostname": "zigbee-eth-bridge"})


def answer(fn):
    async def h(_r):
        return web.json_response(fn())
    return h


async def events(r):
    since = int(r.query.get("since", "0") or 0)
    return web.json_response({"events": [e for e in sim.events if e["id"] > since], "uptime": sim.up()})


async def log(_r):
    return web.json_response({"next": len(LOG), "text": LOG})


async def ok(_r):
    return web.json_response({"ok": True})


async def config_post(r):
    sim.cfg.update({k: v for k, v in (await r.json()).items() if k in sim.cfg})
    return web.json_response({**sim.cfg, "reboot_required": False})


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

DFW = {"max_size": 2093056, "baud": 115200, "busy": False, "dongle": True, "z2m_connected": True,
       "current": "EZSP v19, stack 9.1.1.0", "image": None, "flash": {"state": "idle"}}
FLASH_T0 = [0.0]
B = "sonoff_dongle-pmg24_zigbee_ncp_2026.6.2_9.1.2_460800_"
CAT = {"checking": False, "checked": 0, "checked_ago": -1, "error": "", "repo": "Nerivec/silabs-firmware-builder", "release": "",
       "prerelease": False, "board": "sonoff_dongle-pmg24", "model": "SONOFF Dongle Plus MG24", "current": "9.1.1.0", "builds": [],
       "download": {"state": "idle", "name": "", "done": 0, "total": 0, "error": ""}}
CAT_T = [0.0, 0.0]  # check started, download started
DFW["catalog"] = CAT


def cat_tick():
    if CAT["checking"] and time.time() - CAT_T[0] > 2:
        CAT.update(checking=False, checked_ago=0, release="v2026.6.2-pre1", prerelease=True, builds=[
            {"name": B + "sw_flow.gbl", "version": "9.1.2", "sdk": "2026.6.2", "baud": 460800, "flow": "sw_flow", "size": 270232,
             "sha256": "a" * 64, "usable": True, "recommended": True, "vs_current": "newer"},
            {"name": B + "hw_flow.gbl", "version": "9.1.2", "sdk": "2026.6.2", "baud": 460800, "flow": "hw_flow", "size": 270232,
             "sha256": "b" * 64, "usable": False, "recommended": False, "vs_current": "newer"}])
    d = CAT["download"]
    if d["state"] == "running":
        t = time.time() - CAT_T[1]
        d["done"] = min(d["total"], int(d["total"] * t / 4))
        if t >= 4:
            d["state"] = "done"
            DFW["image"] = {"name": d["name"], "size": d["total"], "upload_size": d["total"], "md5": "0" * 32, "sha256": "a" * 64,
                            "uploaded": int(time.time()), "signed": False, "encrypted": False, "fw_type": "zigbee_ncp",
                            "version": "9.1.2", "sdk": "2026.6.2", "image_baud": 460800}


async def fw_check(_r):
    CAT["checking"] = True
    CAT_T[0] = time.time()
    return web.json_response(DFW)


async def fw_download(r):
    name = (await r.json()).get("name", "")
    b = next((x for x in CAT["builds"] if x["name"] == name), None)
    if not b:
        return web.json_response({"error": "That file isn't in the last check: check again"}, status=409)
    CAT["download"] = {"state": "running", "name": name, "done": 0, "total": b["size"], "error": ""}
    CAT_T[1] = time.time()
    return web.json_response(DFW)


MODELS = [{"id": "sonoff_dongle-pmg24", "name": "SONOFF Dongle Plus MG24", "usb_id": "10c4:ea60", "usb_chip": "CP210x", "radio": "EFR32MG24",
           "family": "Silicon Labs", "protocol": "EmberZNet (EZSP)", "baud": 115200, "rtscts_wired": False, "bootloader_entry": "DTR/RTS reset",
           "flashable": True, "note": "Software flow control only (no RTS/CTS)"},
          {"id": "sonoff_zbdongle-p", "name": "SONOFF ZBDongle-P", "usb_id": "10c4:ea60", "usb_chip": "CP210x", "radio": "CC2652P",
           "family": "Texas Instruments", "protocol": "Z-Stack (ZNP)", "baud": 115200, "rtscts_wired": False,
           "bootloader_entry": "TI BSL (SONOFF gated DTR/RTS)", "flashable": False, "note": ""},
          {"id": "conbee_2", "name": "ConBee II", "usb_id": "1cf1:0030", "usb_chip": "CDC-ACM", "radio": "ATSAMR21",
           "family": "dresden elektronik", "protocol": "deCONZ", "baud": 115200, "rtscts_wired": False,
           "bootloader_entry": "deCONZ bootloader command", "flashable": False, "note": "deCONZ firmware (.GCF); flashing not supported here yet"}]


async def dongle_get(_r):
    cat_tick()
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
            f.update(state="failed" if fail else "done", before="EZSP v19, stack 9.1.1.0",
                     after=None if fail else "EZSP v19, stack 9.1.2.0", dongle_left="bootloader" if fail else None,
                     error="The upload kept failing at 62% (no answer)" if fail else None)
            f = {k: v for k, v in f.items() if v is not None}
            DFW["flash"] = f
            DFW["busy"] = False
            DFW["z2m_connected"] = True
    return web.json_response(DFW)


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
    return web.json_response(DFW)


async def dongle_delete(_r):
    DFW["image"] = None
    return web.json_response(DFW)


async def dongle_flash(r):
    j = await r.json()
    if not DFW["image"] or j.get("md5") != DFW["image"]["md5"]:
        return web.json_response({"error": "That isn't the stored image (MD5 differs): reload the page"}, status=409)
    FLASH_T0[0] = time.time()
    DFW["flash"] = {"state": "running", "step": "image", "done": 0, "total": 0, "seconds": 0}
    DFW["busy"] = True
    DFW["z2m_connected"] = False
    return web.json_response(DFW)


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
    headers = {"Content-Disposition": 'attachment; filename="zigbee-bridge-log.txt"'} if r.query.get("download") == "1" else {}
    return web.Response(text=t, content_type="text/plain", headers=headers)


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
app.router.add_post("/api/dongle/image", dongle_image)
app.router.add_post("/api/dongle/image/delete", dongle_delete)
app.router.add_post("/api/dongle/flash", dongle_flash)
app.router.add_get("/api/update", update_get)
app.router.add_post("/api/update/check", ok)
app.router.add_post("/api/update/install", update_install)
app.router.add_get("/api/state", state)
app.router.add_get("/api/status", answer(status_with_flow))
async def history(r):
    h = sim.history()
    if r.query.get("res") != "10":
        return web.json_response(h)
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
    return web.json_response(out)


app.router.add_get("/api/history", history)
app.router.add_get("/api/devices", answer(sim.devices))
app.router.add_get("/api/config", answer(lambda: sim.cfg))
app.router.add_post("/api/config", config_post)
app.router.add_get("/api/events", events)
app.router.add_get("/api/log", log)
for p in ("/api/reboot", "/api/stats/reset", "/api/logout"):
    app.router.add_post(p, ok)
web.run_app(app, host="127.0.0.1", port=int(sys.argv[1]) if len(sys.argv) > 1 else 18300)
