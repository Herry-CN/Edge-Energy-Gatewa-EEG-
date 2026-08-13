"""Read the STM32 debug UART while poking the board over MQTT.

Tells apart the two candidate explanations for "uplink alive, downlink dead":
  - STM32 is RX-deaf on USART3 (its own publishes would report FAILED even
    though the broker keeps receiving them)
  - the ESP8266 dropped its subscriptions (publishes report OK, no URC arrives)
"""
import json
import sys
import threading
import time

import paho.mqtt.client as mqtt
import serial

PORT_NAME = "COM6"
BAUD = 115200
WATCH = 55.0
POKE_AT = 8.0
BURST = int(sys.argv[1]) if len(sys.argv) > 1 else 1   # 连发条数：>队列深度可验证丢帧计数

BROKER, MQTT_PORT, USER, PASSWD = "192.168.8.97", 1883, "charge", "123456"
T_DEV = "eeg/site01/gw001/charger/ch001"
T_CMD, T_ACK = f"{T_DEV}/cmd", f"{T_DEV}/ack"

t_start = time.time()


def stamp():
    return f"{time.time() - t_start:6.2f}s"


def make_client(cid):
    try:
        return mqtt.Client(mqtt.CallbackAPIVersion.VERSION1, client_id=cid)
    except AttributeError:
        return mqtt.Client(client_id=cid)


def main():
    try:
        ser = serial.Serial(PORT_NAME, BAUD, timeout=0.2)
    except Exception as exc:                    # noqa: BLE001
        print(f"无法打开 {PORT_NAME}: {exc}")
        print("（如果串口助手/MQTTX 之外还开着终端占用它，先关掉再跑）")
        return 2

    print(f"opened {PORT_NAME} @ {BAUD}, watching {WATCH:.0f}s, "
          f"poking at t={POKE_AT:.0f}s")

    def on_message(client, userdata, msg):
        print(f"[{stamp()}] MQTT<- {msg.topic}  {msg.payload.decode('utf-8', 'replace')}")

    client = make_client("eeg-serial-probe")
    client.username_pw_set(USER, PASSWD)
    client.on_message = on_message
    client.connect(BROKER, MQTT_PORT, 30)
    client.subscribe(T_ACK, 0)
    client.loop_start()

    poked = False
    buf = b""
    while time.time() - t_start < WATCH:
        chunk = ser.read(512)
        if chunk:
            buf += chunk
            while b"\n" in buf:
                line, buf = buf.split(b"\n", 1)
                text = line.decode("gbk", "replace").rstrip("\r")
                if text.strip():
                    print(f"[{stamp()}] UART  {text}")
        if not poked and time.time() - t_start >= POKE_AT:
            poked = True
            for n in range(BURST):
                payload = json.dumps({"id": 9500 + n, "action": "set_charge_power",
                                      "value": 10}, separators=(",", ":"))
                print(f"[{stamp()}] MQTT-> {T_CMD}  {payload}")
                client.publish(T_CMD, payload, qos=0, retain=False)

    ser.close()
    client.loop_stop()
    client.disconnect()
    return 0


if __name__ == "__main__":
    sys.exit(main())
