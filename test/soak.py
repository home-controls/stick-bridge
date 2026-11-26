"""Soak: a realistic load (two dashboards and Zigbee Studio polling, plus TCP probes on the Zigbee port)
for a while, sampling memory every 30 s. Passes when nothing failed, the bridge never restarted and free
memory has no downward trend.

  python soak.py <host> <password-file> [minutes=10]
"""
import socket
import sys
import threading
import time

import requests

HOST, PW = sys.argv[1], open(sys.argv[2]).read().strip()
MIN = float(sys.argv[3]) if len(sys.argv) > 3 else 10
B, X = f"http://{HOST}", {"X-Bridge": "1"}
stop = time.time() + MIN * 60
fails, reqs = [0], [0]


def login():
    s = requests.Session()
    s.post(f"{B}/api/login", json={"password": PW}, headers=X, timeout=10).raise_for_status()
    return s


def dashboard(s):
    n = 0
    while time.time() < stop:
        for path in ("/api/status", "/api/events?since=0") + (("/api/history", "/api/devices") if n % 3 == 0 else ()):
            try:
                fails[0] += s.get(B + path, timeout=10).status_code != 200
            except requests.RequestException:
                fails[0] += 1
            reqs[0] += 1
        n += 1
        time.sleep(2)


def studio(s):
    while time.time() < stop:
        for path in ("status", "history", "events", "devices", "config"):
            try:
                fails[0] += s.get(f"{B}/api/{path}", timeout=10).status_code != 200
            except requests.RequestException:
                fails[0] += 1
            reqs[0] += 1
        time.sleep(5)


def probes():
    while time.time() < stop:
        try:
            socket.create_connection((HOST, 6638), timeout=3).close()
        except OSError:
            pass
        time.sleep(7)


s = login()
threads = [threading.Thread(target=dashboard, args=(login(),)), threading.Thread(target=dashboard, args=(login(),)),
           threading.Thread(target=studio, args=(login(),)), threading.Thread(target=probes)]
for t in threads:
    t.start()
samples = []
while time.time() < stop:
    st = s.get(f"{B}/api/status", timeout=10).json()
    samples.append((st["uptime"], st["sys"]["heap_free"], st["sys"]["heap_min"]))
    print(f"  uptime {st['uptime']:>6} s  heap {st['sys']['heap_free']:>7}  min {st['sys']['heap_min']:>7}  "
          f"requests {reqs[0]}  failed {fails[0]}", flush=True)
    time.sleep(30)
for t in threads:
    t.join()
ups = [x[0] for x in samples]
restarted = any(b < a for a, b in zip(ups, ups[1:]))
half = len(samples) // 2
first = sum(x[1] for x in samples[1:half]) / max(1, half - 1)
last = sum(x[1] for x in samples[half:]) / max(1, len(samples) - half)
ok = fails[0] == 0 and not restarted and last > first - 4096
print(f"{'PASS' if ok else 'FAIL'}  soak {MIN:g} min: {reqs[0]} requests, {fails[0]} failed, restarted: {restarted}, "
      f"heap first half {first:.0f} -> second half {last:.0f}, lowest ever {min(x[2] for x in samples)}")
sys.exit(0 if ok else 1)
