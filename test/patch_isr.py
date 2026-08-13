"""Byte-level patch of the GB2312-encoded USART3 ISR.

Editing this file through a text editor rewrites it as UTF-8 and destroys every
Chinese comment in it, so the three edits are done as raw byte splices with
ASCII-only replacement text. Every anchor must match exactly once or nothing is
written.
"""
import sys

PATH = "User/stm32f1xx_it.c"

EDITS = [
    # 1. last_occurrence() has no callers once the ISR stops hand-rolling the scan.
    (b"/**\r\n  * @brief  Last occurrence of needle in hay",
     b"    return hit;\r\n}\r\n\r\n",
     b""),

    # 2. hand-rolled single-slot capture -> queue harvest
    (b"            /* Scan from the LAST +MQTTSUBRECV in the buffer",
     b"                g_mqtt_rx_pending = 1;\r\n            }\r\n",
     b"            /* Slice every complete +MQTTSUBRECV out of the buffer and\r\n"
     b"             * queue it. The queue is what decouples capture from\r\n"
     b"             * processing: dispatching a command publishes an ACK, which\r\n"
     b"             * costs an AT round trip, and anything that arrived during\r\n"
     b"             * that window used to be dropped - or worse, latched the\r\n"
     b"             * downlink dead. Frame boundaries come from the URC's own\r\n"
     b"             * length field, so a payload carrying braces or commas no\r\n"
     b"             * longer confuses the split, and several commands sitting in\r\n"
     b"             * one IDLE frame are all recovered instead of just the last. */\r\n"
     b"            MQTT_RxQueue_Harvest();\r\n"),

    # 3. inline high-water wipe -> shared reset (harvests first, clears the scan mark)
    (b"            strEsp8266_Fram_Record .InfBit .FramLength     = 0;\r\n",
     b"            strEsp8266_Fram_Record .Data_RX_BUF [ 0 ]      = '\\0';\r\n",
     b"            ESP8266_ATFrame_Reset();\r\n"),
]


def main():
    raw = open(PATH, "rb").read()
    before = len(raw)

    for n, (start_tag, end_tag, replacement) in enumerate(EDITS, 1):
        if raw.count(start_tag) != 1:
            print(f"edit {n}: start anchor matched {raw.count(start_tag)} times, aborting")
            return 1
        i = raw.index(start_tag)
        j = raw.find(end_tag, i)
        if j < 0:
            print(f"edit {n}: end anchor not found after start, aborting")
            return 1
        j += len(end_tag)
        raw = raw[:i] + replacement + raw[j:]
        print(f"edit {n}: replaced {j - i} bytes with {len(replacement)}")

    # A GB2312 file must still decode as GB2312 and must not contain U+FFFD.
    try:
        raw.decode("gb2312")
    except UnicodeDecodeError as exc:
        print(f"refusing to write: result no longer decodes as GB2312 ({exc})")
        return 1
    if b"\n" in raw.replace(b"\r\n", b""):
        print("refusing to write: stray LF introduced")
        return 1

    open(PATH, "wb").write(raw)
    print(f"\n{PATH}: {before} -> {len(raw)} bytes, still GB2312, CRLF intact")
    return 0


if __name__ == "__main__":
    sys.exit(main())
