"""Host tests for the update trains (main/updtrain_core.c, fed through fwcat_core.c).

Builds updtrain_harness.c with ASan + UBSan in WSL: picking the newest release with a manifest.json from
a GitHub releases list (RCs, a release after its RCs, hotfixes, refusals), fed in chunks of any size.

  python test_updtrain.py
"""
import subprocess
import sys
from pathlib import Path

HERE = Path(__file__).resolve().parent
MAIN = HERE.parent.parent / "main"
EXE = "/tmp/updtrain_harness"


def wsl_path(p: Path) -> str:
    if sys.platform != "win32":  # Linux (CI): run_all.py puts linux/wsl in front, paths stay as they are
        return p.resolve().as_posix()
    s = str(p.resolve()).replace("\\", "/")
    return f"/mnt/{s[0].lower()}{s[2:]}"


def wsl(*args: str) -> subprocess.CompletedProcess:
    return subprocess.run(["wsl", "--", *args], capture_output=True, text=True, check=False)


def main() -> int:
    r = wsl("gcc", "-std=c11", "-O1", "-g", "-Wall", "-Wextra", "-Werror", "-fsanitize=address,undefined",
            "-fno-sanitize-recover=all", "-I", wsl_path(MAIN), wsl_path(HERE / "updtrain_harness.c"),
            wsl_path(MAIN / "updtrain_core.c"), wsl_path(MAIN / "version_core.c"), wsl_path(MAIN / "fwcat_core.c"), "-o", EXE)
    if r.returncode:
        print(r.stdout, r.stderr)
        return 1
    r = wsl(EXE, "selftest")
    print(r.stdout.strip())
    if r.returncode:
        print(r.stderr)
    return r.returncode


if __name__ == "__main__":
    sys.exit(main())
