"""Dump raw bytes from the debug UART, including partial lines.

The line-oriented probe hides two failure modes this one exposes: a board that
resets repeatedly but dies before finishing its banner, and a board that emits
a few bytes of garbage at some slow interval.
"""
import sys
import time

import serial

PORT_NAME = "COM6"
BAUD = 115200
WATCH = float(sys.argv[1]) if len(sys.argv) > 1 else 90.0


def main():
    try:
        ser = serial.Serial(PORT_NAME, BAUD, timeout=0.2)
    except Exception as exc:                    # noqa: BLE001
        print(f"无法打开 {PORT_NAME}: {exc}")
        return 2

    print(f"raw capture on {PORT_NAME} @ {BAUD} for {WATCH:.0f}s")
    t0 = time.time()
    total = 0
    while time.time() - t0 < WATCH:
        chunk = ser.read(4096)
        if chunk:
            total += len(chunk)
            text = chunk.decode("gbk", "replace")
            print(f"[{time.time() - t0:6.2f}s] +{len(chunk)}B  {text!r}")
    ser.close()

    print(f"\n共收到 {total} 字节")
    if total == 0:
        print("完全静默：MCU 没有在执行 printf 路径（既没运行，也没在复位循环里输出）")
    return 0


if __name__ == "__main__":
    sys.exit(main())
