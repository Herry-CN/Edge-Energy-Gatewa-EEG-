"""Block until the gateway starts publishing live (non-retained) status again.

Used right after a reflash: retained values stay on the broker while the board
is powered down, so only a fresh non-retained report proves it is really back.
"""
import sys
import time

import paho.mqtt.client as mqtt

BROKER, PORT, USER, PASSWD = "192.168.8.97", 1883, "charge", "123456"
T_STATUS = "eeg/site01/gw001/charger/ch001/status"
T_GW = "eeg/site01/gw001/gateway/gw001/status"
TIMEOUT = float(sys.argv[1]) if len(sys.argv) > 1 else 90.0

live = []


def make_client(cid):
    try:
        return mqtt.Client(mqtt.CallbackAPIVersion.VERSION1, client_id=cid)
    except AttributeError:
        return mqtt.Client(client_id=cid)


def main():
    def on_message(client, userdata, msg):
        if msg.retain:
            return
        payload = msg.payload.decode("utf-8", "replace")
        print(f"[{time.strftime('%H:%M:%S')}] {msg.topic}\n    {payload}")
        live.append(msg.topic)

    client = make_client("eeg-wait-online")
    client.username_pw_set(USER, PASSWD)
    client.on_message = on_message
    client.connect(BROKER, PORT, 30)
    client.subscribe("eeg/#", 0)
    client.subscribe("/device/#", 0)
    client.loop_start()

    print(f"等待实时上报，最多 {TIMEOUT:.0f}s ...")
    t0 = time.time()
    while time.time() - t0 < TIMEOUT and len(live) < 3:
        time.sleep(0.2)
    client.loop_stop()
    client.disconnect()

    if len(live) >= 3:
        print(f"\n板子在线（{time.time() - t0:.0f}s 内收到 {len(live)} 条实时上报）")
        return 0
    print(f"\n仍未上线：{TIMEOUT:.0f}s 内只收到 {len(live)} 条实时报文")
    return 1


if __name__ == "__main__":
    sys.exit(main())
