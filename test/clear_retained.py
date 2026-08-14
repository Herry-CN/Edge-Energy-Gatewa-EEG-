"""Delete stale retained messages left behind by earlier firmware builds.

Publishing a zero-length retained payload is how MQTT removes a retained value.
"""
import sys
import time

import paho.mqtt.client as mqtt

BROKER, PORT, USER, PASSWD = "192.168.8.97", 1883, "charge", "123456"

STALE = [
    "eeg/site01/gw001/charger/PILE-001/status",
]


def make_client(cid):
    try:
        return mqtt.Client(mqtt.CallbackAPIVersion.VERSION1, client_id=cid)
    except AttributeError:
        return mqtt.Client(client_id=cid)


def main():
    seen = []

    def on_message(client, userdata, msg):
        if msg.retain and msg.payload:
            seen.append((msg.topic, msg.payload.decode("utf-8", "replace")))

    client = make_client("eeg-retained-cleaner")
    client.username_pw_set(USER, PASSWD)
    client.on_message = on_message
    client.connect(BROKER, PORT, 30)
    client.subscribe("eeg/#", 0)
    client.subscribe("/device/#", 0)
    client.loop_start()
    time.sleep(2.0)

    print("清理前的 retained:")
    for t, p in seen:
        print(f"  {t}\n     {p}")

    for topic in STALE:
        client.publish(topic, payload=b"", qos=1, retain=True)
        print(f"\n已发空 retained 清除: {topic}")
    time.sleep(1.5)

    seen.clear()
    client.unsubscribe("eeg/#")
    client.unsubscribe("/device/#")
    time.sleep(0.3)
    client.subscribe("eeg/#", 0)
    client.subscribe("/device/#", 0)
    time.sleep(2.5)

    print("\n清理后的 retained:")
    for t, p in seen:
        print(f"  {t}\n     {p}")
    left = [t for t, _ in seen if t in STALE]
    print("\n结果: " + ("仍有残留 " + ", ".join(left) if left else "陈旧 retained 已清除"))

    client.loop_stop()
    client.disconnect()
    return 1 if left else 0


if __name__ == "__main__":
    sys.exit(main())
