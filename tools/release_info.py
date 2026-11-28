"""The version this repository is at, checked in every file that states it, and its release notes.

    python tools/release_info.py                     # prints the version; fails if the files disagree
    python tools/release_info.py --tag v1.2.3        # ...or if the tag isn't that version (prints the tag's)
    python tools/release_info.py --notes notes.md    # writes this version's CHANGELOG.md section

The files state the version being prepared, always a plain x.y.z. Tags decide what gets published (see
docs/RELEASING.md): v1.2.3-rc1, v1.2.3-rc2, ... are release candidates of the files' 1.2.3 (GitHub
pre-releases), and v1.2.3 is the release itself, usually on the same commit as the last release candidate.
A release candidate's notes are its own '## x.y.z-rcN' section if the changelog has one, else the
'## x.y.z' section, prefixed with "Release candidate N of x.y.z, for testing."

The release workflow runs it first: a tag that doesn't match the files, or a version without a
changelog entry, releases nothing. Standard library only, the same script in every Zigbee app repo
except for the settings below.
"""

import argparse
import json
import re
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent

# ---- this repository: each file that states the version, and the regex (group 1) or "json" that finds it
VERSIONS = [
    ('version.txt', '^(\\S+)\\s*$'),
]
CHANGELOG = 'CHANGELOG.md'
NOTES_MAX = 500  # bytes: the bridge shows (and keeps) only that much before installing
# ----

PLAIN = re.compile(r"^\d+\.\d+\.\d+$")  # what the files state
RC = re.compile(r"^(\d+\.\d+\.\d+)-rc([1-9]\d*)$")


def stated(path: str, how: str) -> str:
    text = (ROOT / path).read_text(encoding="utf-8")
    if how == "json":
        return str(json.loads(text).get("version", ""))
    m = re.search(how, text, re.M)
    return m.group(1).strip() if m else ""


def notes_for(version: str) -> str:
    """The text under '## <version>' in the changelog, up to the next '## '."""
    text = (ROOT / CHANGELOG).read_text(encoding="utf-8").replace("\r\n", "\n")
    m = re.search(rf"^## {re.escape(version)}[ \t]*\n(.*?)(?=^## |\Z)", text, re.M | re.S)
    return m.group(1).strip() if m else ""


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--tag", help="the release tag: v<version>, or v<version>-rcN for a release candidate")
    ap.add_argument("--notes", type=Path, help="write the release notes (this version's changelog section) here")
    a = ap.parse_args()

    found = {path: stated(path, how) for path, how in VERSIONS}
    versions = set(found.values())
    problems = []
    if len(versions) != 1 or "" in versions:
        problems.append("the files disagree: " + ", ".join(f"{p} says {v or 'nothing'}" for p, v in found.items()))
    version = next(iter(versions)) if len(versions) == 1 else ""
    if version and not PLAIN.match(version):
        problems.append(f"{version} isn't a plain x.y.z version (release candidates come from the tag: vx.y.z-rcN)")
    if a.tag is not None and version:
        tagged = a.tag[1:] if a.tag.startswith("v") else ""
        m = RC.match(tagged)
        if tagged != version and not (m and m.group(1) == version):
            problems.append(f"the tag is {a.tag}, the files say {version} (tag v{version}, or v{version}-rcN)")
        else:
            version = tagged  # what gets published: the release, or one of its release candidates
    notes = notes_for(version) if version else ""
    rc = RC.match(version) if version else None
    if rc and not notes:
        notes = notes_for(rc.group(1))
    if version and not notes:
        problems.append(f"{CHANGELOG} has no '## {version}' entry" + (f" (or '## {rc.group(1)}')" if rc else ""))
    elif rc:
        notes = f"Release candidate {rc.group(2)} of {rc.group(1)}, for testing. " + notes
    if NOTES_MAX and len(notes.encode()) > NOTES_MAX:
        problems.append(f"the {version} notes are {len(notes.encode())} bytes; keep them under {NOTES_MAX}")
    if problems:
        for p in problems:
            print(f"release_info: {p}", file=sys.stderr)
        return 1
    if a.notes:
        a.notes.write_text(notes + "\n", encoding="utf-8", newline="\n")
    print(version)
    return 0


if __name__ == "__main__":
    sys.exit(main())
