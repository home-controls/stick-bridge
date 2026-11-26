"""One vocabulary in what people read: a radio is a slot with its TCP port, a stick is the USB device on it.

Fails on "dongle" in user-facing text: C string literals with a space in them (events, API errors, log lines)
in main/*.c, and string literals or JSX text in web/src/**/*.tsx. Keys, ids, paths and metric names (no
space), product names ("SONOFF Dongle Plus MG24", "nRF52840 Dongle") and dongles.json are fine.

  python test_vocab.py
"""
import re
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
ALLOWED = ["Dongle Plus", "Dongle Lite", "Dongle Max", "ZBDongle", "nRF52840 Dongle", "dongles.json", "Dongle-E", "Dongle-P"]
WORD = re.compile(r"\bdongles?\b", re.I)
C_STR = re.compile(r'"((?:[^"\\\n]|\\.)*)"')
TS_STR = re.compile(r'"((?:[^"\\\n]|\\.)*)"|\'((?:[^\'\\\n]|\\.)*)\'|`((?:[^`\\]|\\.)*)`', re.S)
JSX_TEXT = re.compile(r">([^<>{}]*[A-Za-z][^<>{}]*)<")


def strip_exprs(s: str) -> str:
    """Template expressions are code: drop every ${...}, nested braces included."""
    out, i = [], 0
    while i < len(s):
        if s.startswith("${", i):
            depth, i = 1, i + 2
            while i < len(s) and depth:
                depth += {"{": 1, "}": -1}.get(s[i], 0)
                i += 1
        else:
            out.append(s[i])
            i += 1
    return "".join(out)


def bad_text(s: str) -> bool:
    s = strip_exprs(s)
    if " " not in s.strip():
        return False  # a key, id, path, class name or metric
    for a in ALLOWED:
        s = s.replace(a, "")
    return bool(WORD.search(s))


def strip_c_comments(t: str) -> str:
    t = re.sub(r"/\*.*?\*/", lambda m: "\n" * m.group(0).count("\n"), t, flags=re.S)
    return "\n".join(re.sub(r"//.*$", "", line) if '"' not in line.split("//")[0][-1:] else line for line in t.split("\n"))


def main() -> int:
    hits = []
    for p in sorted((ROOT / "main").glob("*.c")):
        for n, line in enumerate(p.read_text(encoding="utf-8").splitlines(), 1):
            s = line.strip()
            if s.startswith("//") or s.startswith("*") or s.startswith("#include"):
                continue
            for m in C_STR.finditer(line):
                if bad_text(m.group(1)):
                    hits.append(f"{p.relative_to(ROOT)}:{n}: {m.group(1)[:90]}")
    for p in sorted((ROOT / "web" / "src").rglob("*.tsx")):
        text = p.read_text(encoding="utf-8")
        text = re.sub(r"/\*.*?\*/", lambda m: "\n" * m.group(0).count("\n"), text, flags=re.S)
        lines = [re.sub(r"(^|\s)//.*$", "", ln) for ln in text.split("\n")]
        text = "\n".join(lines)
        for m in TS_STR.finditer(text):
            s = next(g for g in m.groups() if g is not None)
            if bad_text(s):
                hits.append(f"{p.relative_to(ROOT)}:{text.count(chr(10), 0, m.start()) + 1}: {s[:90]}")
        for m in JSX_TEXT.finditer(text):
            seg = m.group(1)
            # a comparison in code (a > b ... <) looks like JSX text: real text has no operators or statements
            if any(op in seg for op in ("&&", "||", "=>", "=", ";", "?.", "const ", "return ", "\n")):
                continue
            if bad_text(seg):
                hits.append(f"{p.relative_to(ROOT)}:{text.count(chr(10), 0, m.start()) + 1}: {m.group(1).strip()[:90]}")
    if hits:
        print("\n".join(hits))
        print(f"\n{len(hits)} user-facing 'dongle' left: say 'stick' (the USB device) or 'radio' (the slot)")
        return 1
    print("vocabulary: no user-facing 'dongle' in firmware strings or the dashboard")
    return 0


if __name__ == "__main__":
    sys.exit(main())
