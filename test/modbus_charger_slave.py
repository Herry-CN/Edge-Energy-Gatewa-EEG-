"""Virtual charger (Modbus-RTU slave) for STM32 master bring-up.

Use this on the PC USB-485 (COM7) if you want a slave that actually reacts
to start/stop/power writes. A dumb Modbus Server GUI only stores registers;
this script updates state/voltage/current/power when 1050 / 1024 are written.

    pip install pyserial
    python test/modbus_charger_slave.py --port COM7

Register map: Doc/Modbus 设备模型规范 V1.md
"""
from __future__ import print_function

import argparse
import struct
import sys
import time

try:
    import serial
except ImportError:
    print("pip install pyserial", file=sys.stderr)
    sys.exit(1)

SLAVE = 1
REG_BASE = 1001
REG_LAST = 1050

# holding[reg]  — keys are PDU addresses (same numbers as the spec table)
holding = {}


def crc16(data):
    crc = 0xFFFF
    for b in data:
        crc ^= b
        for _ in range(8):
            crc = (crc >> 1) ^ 0xA001 if crc & 1 else crc >> 1
    return crc


def put(reg, val):
    holding[reg] = val & 0xFFFF


def get(reg):
    return holding.get(reg, 0)


def put_u32(reg, val):
    put(reg, val & 0xFFFF)
    put(reg + 1, (val >> 16) & 0xFFFF)


def get_u32(reg):
    return get(reg) | (get(reg + 1) << 16)


def seed_idle():
    put(1001, 0)          # pile normal
    put(1002, 0)          # fault
    put(1009, 230)        # 23.0 V input (mbserver 401010)
    put(1010, 32)         # 0.32 A
    put(1011, 7)          # 0.7 kW
    put(1023, 0)          # enable
    put(1024, 100)        # target 10.0 kW
    put(1028, 0)          # output V
    put(1029, 0)
    put(1030, 0)
    put(1031, 0)          # soc
    put(1034, 1)          # standby
    put_u32(1035, 0)
    put_u32(1037, 0)
    put(1039, 0)          # raw 0 → MQTT -40 °C
    put(1049, 0)          # chg
    put(1050, 0)


def apply_start():
    p = get(1024) or 100
    u = 3801
    i = (p * 100000 // u) if u else 0
    put(1050, 1)
    put(1023, 1)
    put(1001, 0)          # pile remains normal
    put(1034, 0)          # 启机
    put(1028, u)
    put(1029, i)
    put(1030, p)
    print("  -> START  U=380.1V  P=%.1fkW  I=%.2fA" % (p / 10.0, i / 100.0))


def apply_stop():
    put(1050, 0)
    put(1023, 0)
    put(1001, 0)
    put(1034, 1)          # 待机
    put(1028, 0)
    put(1029, 0)
    put(1030, 0)
    print("  -> STOP   idle")


def on_write(reg, val):
    put(reg, val)
    if reg == 1050:
        if val:
            apply_start()
        else:
            apply_stop()
    elif reg == 1024 and get(1050) == 1:
        apply_start()


def tick_charge(dt):
    if get(1050) != 1:
        return
    p = get(1030)
    if p > 0:
        # 0.01 kWh increment ≈ P(0.1kW) * dt(s) / 360
        e = get_u32(1035) + max(1, int(p * dt / 360.0))
        put_u32(1035, e)
    soc = get(1031)
    if soc < 100:
        put(1031, soc + (1 if dt >= 5 else 0))


def reply(frame):
    if len(frame) < 8:
        return None
    if crc16(frame) != 0:
        return None
    addr, fc = frame[0], frame[1]
    if addr != SLAVE:
        return None

    if fc == 0x03:
        reg = (frame[2] << 8) | frame[3]
        num = (frame[4] << 8) | frame[5]
        if num < 1 or num > 125:
            return exception(addr, fc, 3)
        payload = bytearray([addr, fc, num * 2])
        for i in range(num):
            v = get(reg + i)
            payload += struct.pack(">H", v)
        return with_crc(payload)

    if fc == 0x06:
        reg = (frame[2] << 8) | frame[3]
        val = (frame[4] << 8) | frame[5]
        on_write(reg, val)
        return frame[:8]

    return exception(addr, fc, 1)


def exception(addr, fc, code):
    return with_crc(bytearray([addr, fc | 0x80, code]))


def with_crc(body):
    c = crc16(body)
    return bytes(body) + bytes([c & 0xFF, c >> 8])


def hexdump(tag, data):
    print("%s %s" % (tag, " ".join("%02X" % b for b in data)))


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--port", default="COM7")
    ap.add_argument("--baud", type=int, default=9600)
    args = ap.parse_args()

    seed_idle()
    ser = serial.Serial(args.port, args.baud, timeout=0.05)
    print("virtual charger on %s %d 8N1  slave=%d  regs %d..%d" % (
        args.port, args.baud, SLAVE, REG_BASE, REG_LAST))

    buf = bytearray()
    last_rx = time.time()
    last_tick = time.time()
    soc_acc = 0.0

    while True:
        chunk = ser.read(256)
        now = time.time()
        if chunk:
            buf += chunk
            last_rx = now
            continue
        if buf and (now - last_rx) > 0.004:
            frame = bytes(buf)
            buf.clear()
            hexdump("RX", frame)
            rsp = reply(frame)
            if rsp:
                time.sleep(0.002)
                ser.write(rsp)
                hexdump("TX", rsp)
        if now - last_tick >= 1.0:
            dt = now - last_tick
            last_tick = now
            tick_charge(dt)
            if get(1050) == 1:
                soc_acc += dt
                if soc_acc >= 6.0 and get(1031) < 100:
                    put(1031, get(1031) + 1)
                    soc_acc = 0.0


if __name__ == "__main__":
    try:
        main()
    except KeyboardInterrupt:
        print("\nbye")
