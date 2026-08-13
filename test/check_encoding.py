"""Report the on-disk encoding of the C sources before/after editing them.

The project stores Chinese comments as GB2312; an editor that silently rewrites
a file as UTF-8 corrupts every one of them, so this is run on both sides of any
edit to prove the bytes stayed intact.
"""
import hashlib
import sys

FILES = [
    "User/stm32f1xx_it.c",
    "User/ESP8266/bsp_esp8266.c",
    "User/ESP8266/bsp_esp8266.h",
    "User/ESP8266/bsp_esp8266_mqtt.c",
    "User/ESP8266/bsp_esp8266_mqtt.h",
    "User/ESP8266/bsp_esp8266_test.c",
    "User/ESP8266/bsp_eeg_proto.c",
]


def classify(raw):
    try:
        raw.decode("ascii")
        return "ascii"
    except UnicodeDecodeError:
        pass
    utf8_ok = gbk_ok = True
    try:
        raw.decode("utf-8")
    except UnicodeDecodeError:
        utf8_ok = False
    try:
        raw.decode("gb2312")
    except UnicodeDecodeError:
        gbk_ok = False
    if utf8_ok and not gbk_ok:
        return "utf-8"
    if gbk_ok and not utf8_ok:
        return "gb2312"
    if gbk_ok and utf8_ok:
        return "gb2312/utf-8 (ambiguous)"
    return "unknown/mixed"


def main():
    for path in FILES:
        try:
            raw = open(path, "rb").read()
        except OSError as exc:
            print(f"{path:42s} {exc}")
            continue
        enc = classify(raw)
        crlf = raw.count(b"\r\n")
        lone_lf = raw.count(b"\n") - crlf
        digest = hashlib.sha256(raw).hexdigest()[:12]
        print(f"{path:42s} {enc:26s} {len(raw):7d}B  CRLF={crlf:<5d} LF={lone_lf:<4d} {digest}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
