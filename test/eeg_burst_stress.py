"""Send commands back to back and count how many ACKs come back.

The USART3 ISR holds exactly one pending downlink frame (g_mqtt_rx_pending).
Anything that arrives before the main loop consumes that slot is discarded, so
this sweeps the inter-command gap to find the shortest spacing the firmware can
actually keep up with.
"""
import json
import sys
import threading
import time

import paho.mqtt.client as mqtt

BROKER, PORT, USER, PASSWD = "192.168.8.97", 1883, "charge", "123456"
T_DEV = "eeg/site01/gw001/charger/ch001"
T_CMD, T_ACK = f"{T_DEV}/cmd", f"{T_DEV}/ack"

BURST = 5
GAPS = [0.0, 0.05, 0.1, 0.2, 0.35, 0.5, 0.75, 1.0]
DRAIN = 8.0

lock = threading.Lock()
acks = []


def make_client(cid):
    try:
        return mqtt.Client(mqtt.CallbackAPIVersion.VERSION1, client_id=cid)
    except AttributeError:
        return mqtt.Client(client_id=cid)


def on_message(client, userdata, msg):
    if msg.topic == T_ACK:
        with lock:
            acks.append(msg.payload.decode("utf-8", "replace"))


def main():
    client = make_client("eeg-burst-stress")
    client.username_pw_set(USER, PASSWD)
    client.on_message = on_message
    client.connect(BROKER, PORT, 30)
    client.subscribe(T_ACK, 0)
    client.loop_start()
    time.sleep(1.5)

    print(f"每轮连发 {BURST} 条命令，统计回执数")
    print(f"{'gap(ms)':>9} {'acked':>12} {'lost ids':>20}")
    print("-" * 45)

    base = 8000
    summary = []
    for gi, gap in enumerate(GAPS):
        ids = [base + gi * 100 + k for k in range(BURST)]
        with lock:
            acks.clear()

        for cid in ids:
            client.publish(T_CMD, json.dumps({"id": cid, "action": "set_charge_power",
                                              "value": 10}, separators=(",", ":")),
                           qos=0, retain=False)
            if gap:
                time.sleep(gap)

        time.sleep(DRAIN)
        with lock:
            blob = "".join(acks)
        got = [c for c in ids if f'"id":{c}' in blob]
        lost = [c for c in ids if c not in got]
        summary.append((gap, len(got)))
        print(f"{gap * 1000:>9.0f} {f'{len(got)}/{BURST}':>12} "
              f"{','.join(str(c) for c in lost) if lost else '-':>20}")

    client.loop_stop()
    client.disconnect()

    print("-" * 45)
    ok_gap = next((g for g, n in summary if n == BURST), None)
    if ok_gap is None:
        print("即使间隔 1s 也有丢失，问题不止于单槽竞态")
    else:
        print(f"能全部收下的最小间隔: {ok_gap * 1000:.0f} ms")
    return 0


if __name__ == "__main__":
    sys.exit(main())
