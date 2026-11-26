"""Host tests for several radios on one bridge (main/radio_core.c).

Builds radio_harness.c with ASan + UBSan in WSL and runs its scenarios: which radio a USB stick lands on
as sticks come and go behind a hub (J3 alone, boot order, replug on another port, sticks without serials,
duplicate serials, the lowest free radio, Forget), when a radio shows or frees itself, a new
radio's settings, the 1.x migration,, the per-radio link checks (ports, baud, role, name, allowed
host) and the full-speed re-force rate limit.

Then a mutation check: each rule is broken on purpose in a copy of radio_core.c and the harness must
fail, so the scenarios really pin the rules down.

  python test_radio.py
"""
import subprocess
import sys
import tempfile
from pathlib import Path

HERE = Path(__file__).resolve().parent
MAIN = HERE.parent.parent / "main"
EXE = "/tmp/radio_harness"

# (what it breaks, text in radio_core.c, replacement)
MUTATIONS = [
    ("J3 always radio 0", "if (dev->port == 0) {", "if (dev->port == 255) {"),
    ("unknown stick may take a saved radio", "|| !radio_ident_empty(&slots[i].bound)) continue;", ") continue;"),
    ("serial match ignores in-use", "            if (in_use[i]) {\n                serial_clash = true;",
     "            if (0) {\n                serial_clash = true;"),
    ("port match ignored", "if (b->port != dev->port && !(i == 0 && b->port == 0 && !b->serial[0])) continue;", ""),
    ("J3 serial-less leniency gone", "&& !(i == 0 && b->port == 0 && !b->serial[0])", ""),
    ("duplicate serial ignores port", "int pick = matches > 1 ? (same_port != RADIO_NONE ? same_port : first) : first;", "int pick = first;"),
    ("duplicate ports allowed", "links[j].tcp_port == l->tcp_port", "links[j].tcp_port == l->tcp_port + 1"),
    ("port 80 allowed", "l->tcp_port == 80", "l->tcp_port == 81"),
    ("trailing junk in host allowed", "if (strlen(s) != strlen(back)) return false;", ""),
    ("ZHA allowed on radio 1", "if (i == 0 && l->role == RADIO_ROLE_ZHA) {", "if (i == 9 && l->role == RADIO_ROLE_ZHA) {"),
    ("ZHA has no name", "        case RADIO_ROLE_ZHA: return \"ZHA\";\n", ""),
    ("assign skips the lowest free radio", "        *result = RA_NEW;\n        return i;",
     "        *result = RA_NEW;\n        if (i + 1 < RADIO_MAX && !in_use[i + 1] && radio_ident_empty(&slots[i + 1].bound)) return i + 1;\n        return i;"),
    ("ZHA guessed on radio 1", "default: return i == 0 ? RADIO_ROLE_ZIGBEE : RADIO_ROLE_ZHA;", "default: return i < 0 ? RADIO_ROLE_ZIGBEE : RADIO_ROLE_ZHA;"),
    ("allowed host not inherited", "memcpy(l.allow, links[0].allow, sizeof(l.allow));", ";"),
    ("port clash on adopt", "l.tcp_port = radio_pick_port(links, shown_mask, i);", "l.tcp_port = (uint16_t)(RADIO_PORT_BASE + i + 0 * shown_mask);"),
    ("radio 1 hidden", "return i == 0 || present || set_up;", "return (i < 0) || present || set_up;"),
    ("radio 1 without stick hidden", "return i == 0 ? RS_NO_STICK : RS_HIDDEN;", "return i < 0 ? RS_NO_STICK : RS_HIDDEN;"),
    ("missing shown as hidden", "if (set_up) return RS_MISSING;", "if (set_up && 0) return RS_MISSING;"),
    ("unplug frees while flashing", "return !set_up && !flashing;", "return !set_up || (flashing && 0);"),
    ("unplug frees a set-up radio", "return !set_up && !flashing;", "return (!set_up || 1) && !flashing;"),
    ("stale binding never freed", "if (bound[i] && !set_up[i] && !present[i]) m |= 1u << i;", "if (bound[i] && !set_up[i] && !present[i] && i < 0) m |= 1u << i;"),
    ("stale frees set-up radios", "if (bound[i] && !set_up[i] && !present[i])", "if (bound[i] && (!set_up[i] || 1) && !present[i])"),
    ("migration loses set-up", "set_up_out[i] = bound[i];", "set_up_out[i] = false;"),
    ("migration: on radios lose their port", "(was_on[i] ? pass != 0 : pass != 1)", "(pass != 0)"),
    ("migration: off radios listen for anyone", "if (!was_on[i] && links[i].allow[0] == 0 && links[0].allow[0] != 0) {",
     "if (!was_on[i] && links[i].allow[0] == 0 && links[0].allow[0] != 0 && 0) {"),
    ("hidden radio's port checked", "if (!shown) continue;  // a radio nobody uses", "if (shown && 0) continue;  // a radio nobody uses"),
    ("ports txt overflow unchecked", "if (w < 0 || (size_t)w >= n - len) {", "if (w < 0) {"),
    ("no re-force rate limit","now_us - *last_us < RADIO_REFORCE_US", "0"),
    ("same stick by port despite serials", "if (a->serial[0] && b->serial[0]) return strcmp(a->serial, b->serial) == 0;", ""),
]


def wsl_path(p: Path) -> str:
    if sys.platform != "win32":  # Linux (CI): run_all.py puts linux/wsl in front, paths stay as they are
        return p.resolve().as_posix()
    s = str(p.resolve()).replace("\\", "/")
    return f"/mnt/{s[0].lower()}{s[2:]}"


def wsl(*args: str) -> subprocess.CompletedProcess:
    return subprocess.run(["wsl", "--", *args], capture_output=True, text=True, check=False)


def build(core: Path, exe: str, inc: Path) -> subprocess.CompletedProcess:
    return wsl("gcc", "-std=c11", "-O1", "-g", "-Wall", "-Wextra", "-Werror", "-fsanitize=address,undefined",
               "-fno-sanitize-recover=all", "-I", wsl_path(inc), wsl_path(HERE / "radio_harness.c"),
               wsl_path(core), "-o", exe)


def main() -> int:
    r = build(MAIN / "radio_core.c", EXE, MAIN)
    if r.returncode:
        print(r.stdout, r.stderr)
        return 1
    r = wsl(EXE, "selftest")
    print(r.stdout.strip())
    if r.returncode:
        print(r.stderr)
        return r.returncode

    src = (MAIN / "radio_core.c").read_text(encoding="utf-8")
    survived = []
    with tempfile.TemporaryDirectory() as td:
        tmp = Path(td)
        (tmp / "radio_core.h").write_text((MAIN / "radio_core.h").read_text(encoding="utf-8"), encoding="utf-8")
        for name, old, new in MUTATIONS:
            if old not in src:
                print(f"mutation '{name}': text not found in radio_core.c (update the test)")
                return 1
            (tmp / "radio_core.c").write_text(src.replace(old, new, 1), encoding="utf-8")
            exe = "/tmp/radio_harness_mut"
            b = build(tmp / "radio_core.c", exe, tmp)
            if b.returncode:  # a mutation that doesn't compile proves nothing
                print(f"mutation '{name}': does not compile\n{b.stderr}")
                return 1
            if wsl(exe, "selftest").returncode == 0:
                survived.append(name)
    if survived:
        print("mutations the scenarios did not catch: " + ", ".join(survived))
        return 1
    print(f"mutation check: all {len(MUTATIONS)} caught")
    return 0


if __name__ == "__main__":
    sys.exit(main())
