"""Proves each watchdog fires, on a bridge running the test build (version ending "+test").

  python watchdog_test.py <host> <password-file>

For each kind (task, heap, net): arm it, wait for the bridge to go away and come back, check the
reason it reports for the restart.
"""
import sys
import time

import requests

B, PW = f"http://{sys.argv[1]}", sys.argv[2]
X = {"X-Bridge": "1"}
WANT = {"task": "task watchdog", "heap": "free memory critically low", "net": "network unreachable"}
ok_all = True


def login():
    s = requests.Session()
    s.post(f"{B}/api/login", json={"password": open(PW).read().strip()}, headers=X, timeout=10).raise_for_status()
    return s


def up(timeout=10):
    try:
        return requests.get(f"{B}/api/state", timeout=timeout).ok
    except requests.RequestException:
        return False


s = login()
fw = s.get(f"{B}/api/status", timeout=10).json()["fw"]
if not fw["version"].endswith("+test"):
    sys.exit(f"needs the test build, the bridge runs {fw['version']}")
for kind, want in WANT.items():
    s = login()
    before = s.get(f"{B}/api/status", timeout=10).json()["uptime"]
    t0 = time.time()
    r = s.post(f"{B}/api/debug/watchdog?kind={kind}", headers=X, timeout=10)
    assert r.ok, r.text
    went = None
    while time.time() - t0 < 120:  # gone ...
        if not up(3):
            went = time.time() - t0
            break
        time.sleep(1)
    back = None
    while went is not None and time.time() - t0 < 240:  # ... and back
        if up(3):
            back = time.time() - t0
            break
        time.sleep(1)
    st = login().get(f"{B}/api/status", timeout=10).json() if back else {}
    reason = st.get("sys", {}).get("watchdog", "")
    events = [e["text"] for e in login().get(f"{B}/api/events", timeout=10).json()["events"]] if back else []
    good = bool(back) and st["uptime"] < before + 300 and want in reason and any("Restarted by the watchdog" in e for e in events)
    ok_all &= good
    print(f"{'PASS' if good else 'FAIL'}  {kind}: gone after {went and round(went)} s, back after {back and round(back)} s, "
          f"reason '{reason}', reset '{st.get('sys', {}).get('reset_reason')}'")
sys.exit(0 if ok_all else 1)
