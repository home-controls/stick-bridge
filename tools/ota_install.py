"""Install a firmware image on the bridge over the network and wait until it is confirmed.

  python ota_install.py <host> <password-file> [image]     (image defaults to build/zigbee_eth_bridge.bin)
"""
import sys
import time
from pathlib import Path

import requests

host, pw_file = sys.argv[1], sys.argv[2]
img = Path(sys.argv[3]) if len(sys.argv) > 3 else Path(__file__).resolve().parent.parent / "build" / "zigbee_eth_bridge.bin"
B, X = f"http://{host}", {"X-Bridge": "1"}


def login() -> requests.Session:
    s = requests.Session()
    s.post(f"{B}/api/login", json={"password": Path(pw_file).read_text().strip()}, headers=X, timeout=10).raise_for_status()
    return s


s = login()
before = s.get(f"{B}/api/status", timeout=10).json()["fw"]
r = s.post(f"{B}/api/ota", data=img.read_bytes(), headers={**X, "Content-Type": "application/octet-stream"}, timeout=180)
print("upload:", r.status_code, r.json())
r.raise_for_status()
time.sleep(4)
for _ in range(60):
    try:
        if requests.get(f"{B}/api/state", timeout=2).ok:
            break
    except requests.RequestException:
        pass
    time.sleep(1)
s = login()
for _ in range(50):
    fw = s.get(f"{B}/api/status", timeout=10).json()["fw"]
    if not fw["pending_verify"]:
        break
    time.sleep(2)
print(f"{before['version']} ({before['partition']}) -> {fw['version']} ({fw['partition']}), confirmed: {not fw['pending_verify']}")
sys.exit(0 if not fw["pending_verify"] else 1)
