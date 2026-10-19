"""Compile actual battery status functions with simulated register I/O and time.

No hardware is accessed. --expect-regression is only for a preserved old source.
The generated C file records the exact production source used by this run.
"""
import argparse
import hashlib
from pathlib import Path
import re
import subprocess


def function(source, name):
    match = re.search(r"(?m)^[^\n;{}]*\b" + re.escape(name) + r"\([^;{}]*\)\s*\{", source)
    if not match:
        raise ValueError(f"Production function missing: {name}")
    end = source.find("\n}", match.end())
    if end < 0:
        raise ValueError(f"Function closing brace missing: {name}")
    return source[match.start():end + 2]


def main():
    root = Path(__file__).resolve().parents[1]
    parser = argparse.ArgumentParser()
    parser.add_argument("--zig", required=True)
    parser.add_argument("--build", type=Path, required=True)
    parser.add_argument("--source", type=Path, default=root / "components/tab5_hal/tab5_hal.c")
    parser.add_argument("--expect-regression", action="store_true")
    args = parser.parse_args()
    args.build.mkdir(parents=True, exist_ok=True)
    data = args.source.read_bytes()
    source = data.decode("utf-8-sig")
    header = (root / "components/tab5_hal/include/tab5_hal.h").read_text(encoding="utf-8-sig")
    end = header.index("} tab5_battery_status_t;") + len("} tab5_battery_status_t;")
    declaration = header[header.rfind("typedef struct", 0, end):end]
    generated = declaration + "\n"
    if not args.expect_regression:
        begin = source.index("/* Battery filtering begin.")
        end = source.index("/* Battery filtering end. */", begin)
        generated += source[begin:end] + "\n"
    generated += function(source, "tab5_battery_status_read") + "\n"
    (args.build / "battery_production.h").write_text(generated, encoding="utf-8")
    executable = args.build / "charging_status_test.exe"
    command = [args.zig, "cc", "-std=c11", "-O2", "-Wall", "-Wextra", "-Werror", "-I", str(args.build),
               str(root / "tests/charging_status_test.c"), "-o", str(executable)]
    if args.expect_regression:
        command.insert(2, "-DEXPECT_OLD_REGRESSION")
    subprocess.run(command, check=True)
    print(f"Production source: {args.source}", flush=True)
    print(f"SHA256: {hashlib.sha256(data).hexdigest()}", flush=True)
    result = subprocess.run([str(executable)])
    if args.expect_regression:
        assert result.returncode == 1, f"Expected regression failure, got {result.returncode}"
        print("CONFIRMED BEFORE FAILURE: disconnect regression returns charging=true.", flush=True)
    else:
        assert result.returncode == 0, f"Production charging tests failed: {result.returncode}"


if __name__ == "__main__":
    main()
