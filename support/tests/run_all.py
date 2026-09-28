#!/usr/bin/env python3
import subprocess
import sys
import os

TESTS = [
    ("hi", "support/tests/test_hi.txt"),
    ("badecall", "support/tests/test_badecall.txt"),
    ("bufstorm", "support/tests/test_bufstorm.txt"),
    ("storm_overflow", "support/tests/test_storm_overflow.txt"),
]

def main():
    root = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
    print("=== Running All Lab 2 Automated Tests ===")
    print(f"Tree root: {root}\n")

    passed = 0
    failed = 0

    for name, script in TESTS:
        script_path = os.path.join(root, script)
        log_path = os.path.join(root, f"test_{name}.log")
        cmd = [
            sys.executable,
            os.path.join(root, "support", "inject_uart.py"),
            "--tree", root,
            "--script", script_path,
            "--log", log_path,
        ]
        print(f"--> Running test [{name}] ...")
        res = subprocess.run(cmd)
        if res.returncode == 0:
            print(f"[PASS] {name}\n")
            passed += 1
        else:
            print(f"[FAIL] {name}\n")
            failed += 1

    print("=" * 40)
    print(f"Total: {len(TESTS)} | Passed: {passed} | Failed: {failed}")
    if failed == 0:
        print("ALL TESTS PASSED! Ready for lab2 acceptance.")
        sys.exit(0)
    else:
        print("SOME TESTS FAILED! Check logs.")
        sys.exit(1)

if __name__ == "__main__":
    main()
