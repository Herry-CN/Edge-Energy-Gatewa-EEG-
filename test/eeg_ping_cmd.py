"""Send a single EEG command and report whether the ACK comes back.

Smallest possible downlink liveness check - used to tell "the board is wedged"
apart from "that one command raced with something".
"""
import json
import sys
import threading
import time

import paho.mqtt.client as mqtt

BROKER, PORT, USER, PASSWD = "192.168.8.97", 1883, "charge", "123456"
T_DEV = "eeg/site01/gw001/charger/ch001"
T_CMD, T_ACK = f"{T_DEV}/cmd", f"{T_DEV}/ack"

TRIES = int(sys.argv[1]) if len(sys.argv) > 1 else 3
SPACING = 8.0

lock = threading.Lock()
seen = []


def make_client(cid):
    try:
        return mqtt.Client(mqtt.CallbackAPIVersion.VERSION1, client_id=cid)
    except AttributeError:
        return mqtt.Client(client_id=cid)


def main():
    def on_message(client, userdata, msg):
        with lock:
            seen.append((time.time(), msg.payload.decode("utf-8", "replace")))

    client = make_client("eeg-ping")
    client.username_pw_set(USER, PASSWD)
    client.on_message = on_message
    client.connect(BROKER, PORT, 30)
    client.subscribe(T_ACK, 0)
    client.loop_start()
    time.sleep(1.5)

    ok = 0
    for n in range(TRIES):
        cid = 9100 + n
        with lock:
            seen.clear()
        t0 = time.time()
        client.publish(T_CMD, json.dumps({"id": cid, "action": "set_charge_power",
                                          "value": 10}, separators=(",", ":")),
                       qos=0, retain=False)
        hit = None
        while time.time() - t0 < SPACING:
            with lock:
                hit = next((s for s in seen if f'"id":{cid}' in s[1]), None)
            if hit:
                break
            time.sleep(0.05)
        if hit:
            ok += 1
            print(f"  id={cid}  ACK {hit[0] - t0:.2f}s  {hit[1]}")
        else:
            print(f"  id={cid}  LOST")
        time.sleep(max(0.0, SPACING - (time.time() - t0)))

    client.loop_stop()
    client.disconnect()
    print(f"\n下行存活: {ok}/{TRIES}")
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
