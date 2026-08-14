"""Focused probe for the legacy /device/{id}/control downlink path.

Sends the legacy onoff command a few times, on both the single-L and double-L
topics, and prints every frame that comes back so the failure in step [4] of the
loopback test can be classified: not delivered, delivered but not parsed, or
parsed but the reply publish failed.
"""
import json
import sys
import threading
import time

import paho.mqtt.client as mqtt

BROKER, PORT, USER, PASSWD = "192.168.8.97", 1883, "charge", "123456"
LEGACY_ID = "PILE-001"
T_CTRL = f"/device/{LEGACY_ID}/control"
T_CTRLL = f"/device/{LEGACY_ID}/controll"
T_EEG_STATUS = "eeg/site01/gw001/charger/ch001/status"

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


def show(since_idx, label):
    with lock:
        batch = msgs[since_idx:]
    print(f"    -- {label}: {len(batch)} frame(s)")
    for t, topic, payload in batch:
        print(f"       {time.strftime('%H:%M:%S', time.localtime(t))} {topic}\n"
              f"          {payload}")


def mark():
    with lock:
        return len(msgs)


def trial(client, topic, value, tag):
    print(f"\n[{tag}] publish onoff={value} -> {topic}")
    i = mark()
    payload = json.dumps({"type": "command", "name": "onoff", "value": value, "id": tag},
                         separators=(",", ":"))
    client.publish(topic, payload, qos=0, retain=False)
    time.sleep(9)
    show(i, f"9s after {tag}")


def main():
    client = make_client("eeg-legacy-probe")
    client.username_pw_set(USER, PASSWD)
    client.on_message = on_message
    client.connect(BROKER, PORT, 30)
    client.subscribe("/device/#", 0)
    client.subscribe("eeg/#", 0)
    client.loop_start()
    time.sleep(1.5)

    print("baseline: 6s of normal traffic")
    i = mark()
    time.sleep(6)
    show(i, "baseline")

    trial(client, T_CTRL, 0, "A-single-L-start")
    trial(client, T_CTRL, 2, "B-single-L-stop")
    trial(client, T_CTRLL, 0, "C-double-L-start")
    trial(client, T_CTRLL, 2, "D-double-L-stop")

    client.loop_stop()
    client.disconnect()
    return 0


if __name__ == "__main__":
    sys.exit(main())
