"""Run the whole EEG regression suite in order and summarise.

    python test/run_all.py

Order matters: the burst test is deliberately last, because before the downlink
queue fix it left the board unable to receive any command until a power cycle.
"""
import subprocess
import sys
import time

STEPS = [
    ("下行存活检查", "eeg_ping_cmd.py", ["2"]),
    ("协议闭环（56 项断言）", "eeg_loopback_test.py", []),
    ("旧协议命令通路", "eeg_legacy_probe.py", []),
    ("上报周期内相位扫描", "eeg_race_stress.py", []),
    ("并发连发压力", "eeg_burst_stress.py", []),
    ("并发后下行是否仍存活", "eeg_ping_cmd.py", ["3"]),
]


def main():
    results = []
    for name, script, args in STEPS:
        print("\n" + "=" * 70)
        print(f" {name}   ({script})")
        print("=" * 70)
        t0 = time.time()
        rc = subprocess.call([sys.executable, f"test/{script}", *args])
        results.append((name, rc, time.time() - t0))

    print("\n" + "=" * 70)
    print(" 汇总")
    print("=" * 70)
    for name, rc, dt in results:
        print(f"  {'OK  ' if rc == 0 else 'FAIL'}  {name:<28} ({dt:.0f}s)")
    bad = [n for n, rc, _ in results if rc != 0]
    print(f"\n{len(results) - len(bad)}/{len(results)} 项通过")
    return 1 if bad else 0


if __name__ == "__main__":
    sys.exit(main())
