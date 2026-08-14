"""Quantify the downlink command loss rate against the 5 s report cycle.

Each trial waits for a status report (which marks the start of the firmware's
publish burst), sleeps a varying offset, then sends one EEG command and waits
for its ACK. Sweeping the offset maps out where in the cycle commands get
dropped by the single-slot URC capture in the USART3 ISR.
"""
import json
import sys
import threading
import time

import paho.mqtt.client as mqtt

BROKER, PORT, USER, PASSWD = "192.168.8.97", 1883, "charge", "123456"
T_DEV = "eeg/site01/gw001/charger/ch001"
T_STATUS, T_CMD, T_ACK = f"{T_DEV}/status", f"{T_DEV}/cmd", f"{T_DEV}/ack"

ACK_WAIT = 7.0
OFFSETS = [round(0.25 * i, 2) for i in range(20)]      # 0.00 .. 4.75 s

lock = threading.Lock()
msgs = []


def make_client(cid):
    try:
        return mqtt.Client(mqtt.CallbackAPIVersion.VERSION1, client_id=cid)
    except AttributeError:
        return mqtt.Client(client_id=cid)


def on_message(client, userdata, msg):
    with lock:
        msgs.append((time.time(), msg.topic, msg.payload.decode("utf-8", "replace")))


def mark():
    with lock:
        return len(msgs)


def wait_topic(topic, since, timeout, match=None):
    end = time.time() + timeout
    while time.time() < end:
        with lock:
            for m in msgs[since:]:
                if m[1] == topic and (match is None or match(m[2])):
                    return m
        time.sleep(0.02)
    return None


def main():
    client = make_client("eeg-race-stress")
    client.username_pw_set(USER, PASSWD)
    client.on_message = on_message
    client.connect(BROKER, PORT, 30)
    client.subscribe(f"{T_DEV}/#", 0)
    client.loop_start()
    time.sleep(1.5)

    print(f"{'offset(s)':>10} {'result':>8} {'latency(s)':>11}")
    print("-" * 32)

    hits, misses = [], []
    for n, off in enumerate(OFFSETS):
        cmd_id = 7000 + n

        i = mark()
        st = wait_topic(T_STATUS, i, 8.0)
        if st is None:
            print("  no status report - board offline?")
            break
        time.sleep(off)

        i = mark()
        t0 = time.time()
        client.publish(T_CMD, json.dumps({"id": cmd_id, "action": "set_charge_power",
                                          "value": 10}, separators=(",", ":")),
                       qos=0, retain=False)
        ack = wait_topic(T_ACK, i, ACK_WAIT, match=lambda p: f'"id":{cmd_id}' in p)

        if ack:
            lat = ack[0] - t0
            hits.append((off, lat))
            print(f"{off:>10.2f} {'ok':>8} {lat:>11.2f}")
        else:
            misses.append(off)
            print(f"{off:>10.2f} {'LOST':>8} {'-':>11}")

    client.loop_stop()
    client.disconnect()

    total = len(hits) + len(misses)
    print("-" * 32)
    if total:
        print(f"送达 {len(hits)}/{total}，丢失 {len(misses)}/{total} "
              f"({100.0 * len(misses) / total:.0f}%)")
    if hits:
        lats = [l for _, l in hits]
        print(f"ACK 时延 min={min(lats):.2f}s max={max(lats):.2f}s "
              f"avg={sum(lats) / len(lats):.2f}s")
    if misses:
        print("丢失发生在上报后 " + ", ".join(f"{o:.2f}s" for o in misses))
    return 0


if __name__ == "__main__":
    sys.exit(main())
