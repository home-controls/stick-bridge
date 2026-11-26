"""Stress and abuse tests against a live bridge on the LAN (no dongle needed).

  python live_stress.py <host> <password-file>

If the bridge is at first-boot setup, a throwaway password is created and written to the file;
factory-reset the bridge afterwards (System, Factory reset) so its owner sets the real one.
Checks that nothing crashes (uptime keeps growing, no new reset), memory comes back, and every
abuse gets a clean refusal.
"""
import concurrent.futures as cf
import os
import secrets
import socket
import sys
import time

import requests

B = f"http://{sys.argv[1]}"
HOST = sys.argv[1]
PW_FILE = sys.argv[2]
X = {"X-Bridge": "1"}
results: list[tuple[str, bool, str]] = []


def check(name, cond, detail=""):
    results.append((name, bool(cond), str(detail)))
    print(f"{'PASS' if cond else 'FAIL'}  {name}  {detail}", flush=True)


def session() -> requests.Session:
    s = requests.Session()
    st = requests.get(f"{B}/api/state", timeout=5).json()
    if st["setup_required"]:
        pw = secrets.token_urlsafe(12)
        open(PW_FILE, "w").write(pw)
        s.post(f"{B}/api/setup", json={"password": pw}, headers=X, timeout=10).raise_for_status()
    else:
        s.post(f"{B}/api/login", json={"password": open(PW_FILE).read().strip()}, headers=X, timeout=10).raise_for_status()
    return s


def sysinfo(s):
    st = s.get(f"{B}/api/status", timeout=10).json()
    return st["uptime"], st["sys"]["heap_free"], st["sys"]["heap_min"], st["sys"]["reset_reason"]


s = session()
up0, heap0, min0, rr0 = sysinfo(s)
print(f"start: uptime {up0}s heap {heap0} min {min0} last reset {rr0}")

# 1. pages polling: a realistic load (a few dashboards + Zigbee Studio) must never fail; an overload
#    (more clients than the server's 12 sockets, polling flat out) may shed requests but must not hurt the bridge
from collections import Counter
kinds = Counter()


def poll(i, secs=45, pause=0.0):
    errs, n = 0, 0
    end = time.time() + secs
    ss = requests.Session()
    ss.cookies.update(s.cookies)
    paths = ["/api/status", "/api/history", "/api/devices", "/api/events", "/api/log", "/api/config", "/"]
    while time.time() < end:
        try:
            r = ss.get(B + paths[n % len(paths)], timeout=10)
            if r.status_code != 200:
                errs += 1
                kinds[f"HTTP {r.status_code}"] += 1
        except requests.RequestException as ex:
            errs += 1
            kinds[type(ex).__name__] += 1
        n += 1
        time.sleep(pause)
    return n, errs


with cf.ThreadPoolExecutor(6) as ex:
    out = list(ex.map(lambda i: poll(i, 60, 0.5), range(6)))
n, e = sum(o[0] for o in out), sum(o[1] for o in out)
check("realistic: 6 clients polling every 0.5 s for 60 s", e == 0, f"{n} requests, {e} failed {dict(kinds)}")
kinds.clear()
with cf.ThreadPoolExecutor(24) as ex:
    out = list(ex.map(poll, range(24)))
n, e = sum(o[0] for o in out), sum(o[1] for o in out)
print(f"INFO  overload: 24 clients flat out for 45 s: {n} requests, {e} shed ({e / max(n, 1):.1%}) {dict(kinds)}")
time.sleep(2)
check("dashboard answers right after the overload", s.get(f"{B}/api/status", timeout=10).status_code == 200)

# 2. session churn: more logins than session slots
pw = open(PW_FILE).read().strip()
oks = 0
for _ in range(30):
    r = requests.post(f"{B}/api/login", json={"password": pw}, headers=X, timeout=10)
    oks += r.status_code == 200
check("30 logins in a row", oks == 30, f"{oks} ok")
check("our first session was pushed out (6 slots)", s.get(f"{B}/api/status", timeout=10).status_code == 401)
s = session()

# 3. malformed and oversized input
bad = []
r = s.post(f"{B}/api/config", data="{not json", headers={**X, "Content-Type": "application/json"}, timeout=10)
bad.append(("malformed JSON", r.status_code, 400))
r = s.post(f"{B}/api/config", data="x" * 100_000, headers={**X, "Content-Type": "application/json"}, timeout=20)
bad.append(("100 KB body", r.status_code, 400))
r = s.post(f"{B}/api/config", json={"tcp_port": "6638"}, headers=X, timeout=10)
bad.append(("port as a string", r.status_code, 400))
r = s.post(f"{B}/api/config", json={"tcp_port": 70000}, headers=X, timeout=10)
bad.append(("port 70000", r.status_code, 400))
r = s.post(f"{B}/api/config", json={"baud": -5}, headers=X, timeout=10)
bad.append(("baud -5", r.status_code, 400))
r = s.post(f"{B}/api/config", json={"hostname": "a" * 300}, headers=X, timeout=10)
bad.append(("300-char hostname", r.status_code, 400))
r = s.post(f"{B}/api/config", json={"allow": "1.2.3.4/33"}, headers=X, timeout=10)
bad.append(("allow /33", r.status_code, 400))
r = s.post(f"{B}/api/config", json={"ip": "300.1.1.1", "mode": "static"}, headers=X, timeout=10)
bad.append(("ip 300.1.1.1", r.status_code, 400))
r = s.get(f"{B}/api/events?since=abc", timeout=10)
bad.append(("events since=abc", r.status_code, 200))
r = s.get(f"{B}/api/log?since=4294967295", timeout=10)
bad.append(("log since=max", r.status_code, 200))
r = s.get(f"{B}/api/nope", timeout=10)
bad.append(("unknown path", r.status_code, 404))
try:
    r = s.get(f"{B}/api/status?" + "a" * 600, timeout=10)
    bad.append(("600-char query", r.status_code in (200, 404, 414, 431), True))
except requests.RequestException:
    bad.append(("600-char query (connection closed)", True, True))
r = requests.get(f"{B}/api/status", headers={"Authorization": "Basic " + "A" * 900}, timeout=10)
bad.append(("900-char Basic header", r.status_code in (401, 431), True))
r = requests.get(f"{B}/api/status", headers={"Authorization": "Bearer " + "f" * 64}, timeout=10)
bad.append(("wrong Bearer token", r.status_code, 401))
r = requests.get(f"{B}/api/status", headers={"Cookie": "zb_session=" + "0" * 32}, timeout=10)
bad.append(("forged session cookie", r.status_code, 401))
r = s.post(f"{B}/api/password", json={"current": pw, "new": "x" * 65}, headers=X, timeout=10)
bad.append(("65-char password", r.status_code, 400))
for name, got, want in bad:
    check(f"input: {name}", got == want if not isinstance(want, bool) else want is True and got, f"HTTP {got}")

# 4. TCP 6638: a flood of connects (refused: wrong host / no dongle), then some held open
ev_before = max([x["id"] for x in s.get(f"{B}/api/events", timeout=10).json()["events"]] or [0])
t0 = time.time()
for _ in range(300):
    try:
        c = socket.create_connection((HOST, 6638), timeout=3)
        c.close()
    except OSError:
        pass
held = []
for _ in range(20):
    try:
        held.append(socket.create_connection((HOST, 6638), timeout=3))
    except OSError:
        pass
time.sleep(2)
alive = 0
for c in held:
    c.settimeout(1)
    try:
        alive += c.recv(1) != b""
    except socket.timeout:
        alive += 1
    except OSError:
        pass
    c.close()
check("300 quick connects + 20 held to 6638 are all refused", alive == 0, f"{alive} held open by the bridge, {time.time() - t0:.0f} s")
r = s.get(f"{B}/api/events?since={ev_before}", timeout=10).json()["events"]
refusals = [x for x in r if "Refused" in x["text"]]
check("320 refused connects log at most 2 refusal events", len(refusals) <= 2, f"{len(refusals)} refusal events")

# 5. idle HTTP connections (more than the server keeps), then a normal request
idle = []
for _ in range(15):
    try:
        idle.append(socket.create_connection((HOST, 80), timeout=3))
    except OSError:
        pass
time.sleep(1)
try:
    ok = s.get(f"{B}/api/status", timeout=15).status_code == 200
except requests.RequestException as ex:
    ok = False
check("dashboard still answers with 15 idle HTTP connections open", ok, f"{len(idle)} opened")
for c in idle:
    c.close()

# 6. interrupted firmware upload, then a lying Content-Length
img = open(os.path.join(os.path.dirname(__file__), "..", "build", "zigbee_eth_bridge.bin"), "rb").read()
c = socket.create_connection((HOST, 80), timeout=5)
cookie = "; ".join(f"{k}={v}" for k, v in s.cookies.items())
c.sendall((f"POST /api/ota HTTP/1.1\r\nHost: {HOST}\r\nX-Bridge: 1\r\nCookie: {cookie}\r\n"
           f"Content-Type: application/octet-stream\r\nContent-Length: {len(img)}\r\n\r\n").encode() + img[:200_000])
time.sleep(1)
c.close()
time.sleep(4)
ev = [x["text"] for x in s.get(f"{B}/api/events", timeout=10).json()["events"]]
check("interrupted upload aborted cleanly", any("upload failed" in t for t in ev), [t for t in ev if "irmware" in t][-2:])
c = socket.create_connection((HOST, 80), timeout=5)
c.sendall((f"POST /api/ota HTTP/1.1\r\nHost: {HOST}\r\nX-Bridge: 1\r\nCookie: {cookie}\r\n"
           f"Content-Type: application/octet-stream\r\nContent-Length: 99999999\r\n\r\n").encode() + img[:64])
c.settimeout(10)
try:
    resp = c.recv(400).decode(errors="replace")
except OSError:
    resp = ""
c.close()
check("oversized upload refused up front", "400" in resp.split("\r\n")[0] if resp else False, resp.split("\r\n")[0] if resp else "no answer")

# 7. a client that starts a request body and goes quiet must not hold up the dashboard for long
stall = socket.create_connection((HOST, 80), timeout=5)
stall.sendall((f"POST /api/config HTTP/1.1\r\nHost: {HOST}\r\nX-Bridge: 1\r\nCookie: {cookie}\r\n"
               f"Content-Type: application/json\r\nContent-Length: 500\r\n\r\n").encode() + b'{"allow":')
time.sleep(1)
t0 = time.time()
try:
    ok = requests.get(f"{B}/api/state", timeout=30).ok
except requests.RequestException:
    ok = False
check("a stalled request body holds the dashboard up 15 s at most", ok and time.time() - t0 < 15, f"answered after {time.time() - t0:.1f} s")
stall.close()

# 8. still the same boot, memory back
time.sleep(3)
s = session()
up1, heap1, min1, rr1 = sysinfo(s)
check("no reboot during the tests", up1 > up0 and rr1 == rr0, f"uptime {up0} -> {up1}, last reset {rr1}")
check("memory came back", heap1 > heap0 - 16_000, f"heap {heap0} -> {heap1} (lowest ever {min1})")
print(f"\n{sum(ok for _, ok, _ in results)}/{len(results)} passed")
