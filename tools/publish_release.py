"""Publishes the built firmware as a release the bridges update from (System page, Updates).

  python tools/publish_release.py --notes "What changed" --github
  python tools/publish_release.py --notes "What changed" [--repo ../zigbee-ota] [--commit] [--push]

--github makes a GitHub release of this repo (gh CLI) with the three files below, which is where the
bridge looks by default, plus zigbee_eth_bridge-<version>-full.bin (everything at 0x0, for a first flash
with plain esptool) and dongles.json ([] unless release/<version>/ already has one); run it from ESP-IDF's
environment, whose Python has esptool. Otherwise it writes them into <repo>/eth-bridge/ of a git checkout
you serve:
  zigbee_eth_bridge-<version>.bin       the image (build/zigbee_eth_bridge.bin)
  zigbee_eth_bridge-<version>.bin.md5   "<md5>  <file>" (md5sum format)
  manifest.json                          version, file, size, md5, sha256, date, notes
The bridge reads manifest.json, downloads the image and checks size, MD5 and SHA-256 before it
switches to it. Older images stay in the folder: pointing the manifest back at one is a rollback.
"""

import argparse
import datetime
import hashlib
import json
import os
import subprocess
import sys
from pathlib import Path

HERE = Path(__file__).resolve().parent.parent  # the zigbee-bridge repo
IMAGE = HERE / "build" / "zigbee_eth_bridge.bin"
# esp_image_header (24) + first segment header (8), then esp_app_desc_t: magic (4), secure_version (4),
# reserved (8), version (32), project_name (32)
DESC = 24 + 8
APP_DESC_MAGIC = 0xABCD5432


def image_info(data: bytes) -> tuple[str, str]:
    if data[0] != 0xE9:
        sys.exit("not an ESP32 image")
    magic = int.from_bytes(data[DESC:DESC + 4], "little")
    if magic != APP_DESC_MAGIC:
        sys.exit("no app description in the image")
    version = data[DESC + 16:DESC + 48].split(b"\0")[0].decode()
    project = data[DESC + 48:DESC + 80].split(b"\0")[0].decode()
    return version, project


def full_image(out: Path, version: str) -> Path:
    """Bootloader, partition table, OTA data and app as one file, written at 0x0, for a board's first flash
    without ESP-IDF: esptool --chip esp32p4 write_flash 0x0 <file>. Needs esptool (ESP-IDF's Python has it)."""
    target = (out / f"zigbee_eth_bridge-{version}-full.bin").resolve()
    args = (HERE / "build" / "flash_args").read_text().split()  # flash mode/freq/size, then offset + file pairs
    subprocess.run([sys.executable, "-m", "esptool", "--chip", "esp32p4", "merge_bin", "-o", str(target), *args],
                   check=True, cwd=HERE / "build")
    if target.read_bytes()[0x20000:0x20000 + IMAGE.stat().st_size] != IMAGE.read_bytes():
        sys.exit(f"{target.name} doesn't hold build/zigbee_eth_bridge.bin at 0x20000")
    return target


def main() -> None:
    p = argparse.ArgumentParser()
    p.add_argument("--notes", required=True, help="what changed, shown on the bridge before installing")
    p.add_argument("--repo", default=None, help="a git checkout serving the images (its eth-bridge/ folder), "
                   "else ZIGBEE_OTA, else ../zigbee-ota next to this repo")
    p.add_argument("--github", action="store_true", help="publish as a GitHub release of this repo instead (gh CLI): "
                   "the bridge's default update URL is releases/latest/download/manifest.json")
    p.add_argument("--commit", action="store_true", help="git add + commit in the repo")
    p.add_argument("--push", action="store_true", help="git push after committing")
    a = p.parse_args()
    if a.github and (a.repo or a.commit or a.push):
        sys.exit("--github writes to release/ and publishes with gh: no --repo, --commit or --push")
    if len(a.notes.strip().encode()) > 500:
        sys.exit("keep --notes under 500 bytes: the bridge shows (and keeps) only that much")

    want = (HERE / "version.txt").read_text().strip()
    data = IMAGE.read_bytes()
    version, project = image_info(data)
    if project != "zigbee_eth_bridge":
        sys.exit(f"the image is {project!r}, not zigbee_eth_bridge")
    if version != want:
        sys.exit(f"the image says {version}, version.txt says {want}: build again")
    if "+" in version or "-" in version:
        sys.exit(f"{version} is a test build: releases are plain x.y.z")

    if a.github:
        out = HERE / "release" / version
    else:
        a.repo = a.repo or os.environ.get("ZIGBEE_OTA") or str(HERE.parent / "zigbee-ota")
        out = Path(a.repo) / "eth-bridge"
    out.mkdir(parents=True, exist_ok=True)
    name = f"zigbee_eth_bridge-{version}.bin"
    md5 = hashlib.md5(data).hexdigest()
    sha = hashlib.sha256(data).hexdigest()
    (out / name).write_bytes(data)
    (out / f"{name}.md5").write_text(f"{md5}  {name}\n", newline="\n")
    manifest = {"version": version, "file": name, "size": len(data), "md5": md5, "sha256": sha,
                "date": datetime.date.today().isoformat(), "notes": a.notes.strip()}
    (out / "manifest.json").write_text(json.dumps(manifest, indent=2) + "\n", newline="\n")
    print(f"release {version}: {len(data)} bytes, md5 {md5}, sha256 {sha[:16]}...")
    print(f"wrote {out / name}, {name}.md5 and manifest.json")

    if a.github:
        full = full_image(out, version)
        dongles = out / "dongles.json"  # the bridge fetches it next to the manifest; [] = only the built-in list
        if not dongles.exists():
            dongles.write_text("[]\n", newline="\n")
        files = [str(out / name), str(out / f"{name}.md5"), str(out / "manifest.json"), str(full), str(dongles)]
        subprocess.run(["gh", "release", "create", f"v{version}", *files, "--title", f"Zigbee Bridge {version}",
                        "--notes", a.notes.strip()], check=True, cwd=HERE)
        print("published the GitHub release")
        return

    if a.commit:
        git = ["git", "-c", "safe.directory=*", "-C", a.repo]
        subprocess.run(git + ["add", "eth-bridge"], check=True)
        subprocess.run(git + ["commit", "-q", "-m", f"eth-bridge {version}"], check=True)
        print("committed")
        if a.push:
            subprocess.run(git + ["push", "-q"], check=True)
            print("pushed: bridges see it at their next check (or Check now)")


if __name__ == "__main__":
    main()
