"""Host tests for the dongle firmware catalogue (main/fwcat_core.c), built in WSL with ASan + UBSan:
file-name parsing, version compare, variant scoring, and the streaming scanner against real GitHub
releases API responses fed in random-sized pieces (every asset's name, size, sha256 digest and URL
must match Python's json.load).

  python test_fwcat.py [releases.json ...]

With no files it fetches Nerivec/silabs-firmware-builder's ?per_page=2 and /releases/latest into a
temporary folder (public API, no token).
"""
import json
import subprocess
import sys
import tempfile
import urllib.request
from pathlib import Path

HERE = Path(__file__).resolve().parent
MAIN = HERE.parent.parent / "main"
EXE = "/tmp/fwcat_harness"
API = "https://api.github.com/repos/Nerivec/silabs-firmware-builder/releases"


def wsl_path(p: Path) -> str:
    if sys.platform != "win32":  # Linux (CI): run_all.py puts linux/wsl in front, paths stay as they are
        return p.resolve().as_posix()
    s = str(p.resolve()).replace("\\", "/")
    return f"/mnt/{s[0].lower()}{s[2:]}"


def wsl(*args: str) -> subprocess.CompletedProcess:
    return subprocess.run(["wsl", "--", *args], capture_output=True, text=True, check=False)


def expected(path: Path) -> list[str]:
    data = json.loads(path.read_text(encoding="utf-8"))
    rels = data if isinstance(data, list) else [data]
    out = []
    for i, r in enumerate(rels):
        for a in r["assets"]:
            d = a.get("digest") or ""
            sha = d[7:] if d.startswith("sha256:") and len(d) == 71 else ""
            out.append(f"{i}|{r['tag_name'][:47]}|{int(r['prerelease'])}|{a['name'][:111]}|{a['size']}|{sha}|"
                       f"{a['browser_download_url'][:255]}")
    return out


def main() -> int:
    r = wsl("gcc", "-std=gnu11", "-O1", "-g", "-Wall", "-Wextra", "-Werror", "-Wno-unused-variable",
            "-fsanitize=address,undefined", "-fno-sanitize-recover=all", "-I", wsl_path(MAIN),
            wsl_path(HERE / "fwcat_harness.c"), wsl_path(MAIN / "fwcat_core.c"), "-o", EXE)
    if r.returncode:
        print(r.stdout, r.stderr)
        return 1
    r = wsl(EXE, "selftest")
    print(r.stdout.strip())
    bad = r.returncode != 0
    files = [Path(p) for p in sys.argv[1:]]
    tmp = None
    if not files:
        tmp = tempfile.TemporaryDirectory()
        for name, url in (("releases.json", API + "?per_page=2"), ("latest.json", API + "/latest")):
            req = urllib.request.Request(url, headers={"User-Agent": "zigbee-eth-bridge-test",
                                                        "Accept": "application/vnd.github+json"})
            p = Path(tmp.name) / name
            p.write_bytes(urllib.request.urlopen(req, timeout=30).read())
            files.append(p)
    for f in files:
        want = expected(f)
        for seed in (1, 2, 3):
            r = wsl(EXE, "scan", wsl_path(f), str(seed))
            got = r.stdout.splitlines()
            ok = r.returncode == 0 and got == want
            print(f"{'ok  ' if ok else 'FAIL'} scan {f.name} seed {seed}: {len(got)}/{len(want)} assets")
            if not ok:
                bad = True
                print(r.stderr)
                for g, w in zip(got, want):
                    if g != w:
                        print("  got ", g, "\n  want", w)
                        break
    return 1 if bad else 0


if __name__ == "__main__":
    sys.exit(main())
