"""Passive sniffer: connect to the broker and print whatever the gateway says.

Used as the first step of the closed-loop test to find out whether a board is
online at all and which protocol version it is speaking.
"""
import sys
import time

import paho.mqtt.client as mqtt

BROKER = "192.168.8.97"
PORT = 1883
USER = "charge"
PASSWD = "123456"
WATCH = 20.0

TOPICS = [("eeg/#", 0), ("/device/#", 0)]


def make_client(client_id):
    try:
        return mqtt.Client(mqtt.CallbackAPIVersion.VERSION1, client_id=client_id)
    except AttributeError:                      # paho-mqtt 1.x
        return mqtt.Client(client_id=client_id)


def main():
    seen = []

    def on_connect(client, userdata, flags, rc, properties=None):
        print(f"[conn] rc={rc}")
        for t, q in TOPICS:
            client.subscribe(t, q)
            print(f"[sub ] {t}")

    def on_message(client, userdata, msg):
        payload = msg.payload.decode("utf-8", "replace")
        stamp = time.strftime("%H:%M:%S")
        flag = " RETAINED" if msg.retain else ""
        print(f"[{stamp}] {msg.topic}{flag}\n           {payload}")
        seen.append(msg.topic)

    client = make_client("eeg-observer")
    client.username_pw_set(USER, PASSWD)
    client.on_connect = on_connect
    client.on_message = on_message
    client.connect(BROKER, PORT, 30)
    client.loop_start()

    print(f"listening {WATCH:.0f}s ...")
    time.sleep(WATCH)
    client.loop_stop()

    print("\n=== summary ===")
    if not seen:
        print("nothing received - no gateway is publishing right now")
        return 1
    for topic in sorted(set(seen)):
        print(f"{seen.count(topic):3d} x {topic}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
