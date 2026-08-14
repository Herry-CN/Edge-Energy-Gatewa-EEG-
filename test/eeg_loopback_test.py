"""Closed-loop test for the EEG V1.0 gateway firmware.

Drives the real board through the real broker: publishes commands on the §7 cmd
topic, then asserts on the §8 ACK and on the §6 status report that follows.

Prerequisites
    - board flashed and online (run eeg_observe.py first if unsure)
    - this PC on the same LAN as the broker

Usage
    python test/eeg_loopback_test.py
"""
import json
import sys
import threading
import time

import paho.mqtt.client as mqtt

# ---------------------------------------------------------------- config
BROKER = "192.168.8.97"
PORT = 1883
USER = "charge"
PASSWD = "123456"

SITE, GW, DEV_TYPE, DEV = "site01", "gw001", "charger", "ch001"
ROOT = f"eeg/{SITE}/{GW}"
T_GW_STATUS = f"{ROOT}/gateway/{GW}/status"
T_DEV = f"{ROOT}/{DEV_TYPE}/{DEV}"
T_STATUS = f"{T_DEV}/status"
T_CMD = f"{T_DEV}/cmd"
T_ACK = f"{T_DEV}/ack"
T_EVENT = f"{T_DEV}/event"

LEGACY_ID = "PILE-001"
T_LEGACY_STATUS = f"/device/{LEGACY_ID}/status"
T_LEGACY_CTRL = f"/device/{LEGACY_ID}/control"

STATUS_PERIOD = 5.0
ACK_TIMEOUT = 8.0
STATUS_TIMEOUT = 14.0

# ---------------------------------------------------------------- plumbing
_lock = threading.Lock()
_msgs = []          # list of dicts: topic, payload, retain, t

results = []        # (ok, name, detail)


def make_client(client_id):
    try:
        return mqtt.Client(mqtt.CallbackAPIVersion.VERSION1, client_id=client_id)
    except AttributeError:
        return mqtt.Client(client_id=client_id)


def on_message(client, userdata, msg):
    with _lock:
        _msgs.append({
            "topic": msg.topic,
            "payload": msg.payload.decode("utf-8", "replace"),
            "retain": bool(msg.retain),
            "t": time.time(),
        })


def mark():
    """Index of the next message; use it to ignore everything already seen."""
    with _lock:
        return len(_msgs)


def wait_for(topic, since, timeout, want=None):
    """Return the first message on `topic` after index `since` (optionally
    matching predicate `want`), or None on timeout."""
    deadline = time.time() + timeout
    while time.time() < deadline:
        with _lock:
            for m in _msgs[since:]:
                if m["topic"] != topic:
                    continue
                if want is not None and not want(m):
                    continue
                return m
        time.sleep(0.05)
    return None


def check(name, ok, detail=""):
    results.append((ok, name, detail))
    print(f"  [{'PASS' if ok else 'FAIL'}] {name}" + (f"  -> {detail}" if detail else ""))
    return ok


def as_json(msg):
    try:
        return json.loads(msg["payload"])
    except Exception as exc:                    # noqa: BLE001
        return {"__parse_error__": str(exc)}


# ---------------------------------------------------------------- test steps
def step_schema(client):
    print("\n[1] 上报报文结构与字段类型（§5 网关状态 / §6 设备状态）")

    since = mark()
    st = wait_for(T_STATUS, since, STATUS_TIMEOUT)
    if not check("收到设备状态上报", st is not None, T_STATUS):
        return
    d = as_json(st)
    check("设备状态是合法 JSON（>256 字节，走的是 AT+MQTTPUBRAW）",
          "__parse_error__" not in d, d.get("__parse_error__", f"{len(st['payload'])} bytes"))

    want = ["online", "state", "fault_code", "voltage", "current", "power",
            "energy_charge", "soc", "temperature", "mode", "ts"]
    missing = [k for k in want if k not in d]
    check("§6 字段齐全", not missing, f"缺少 {missing}" if missing else f"{len(want)} 个字段")
    check("state 取值合法", d.get("state") in ("charging", "idle", "offline", "fault"),
          str(d.get("state")))
    check("voltage/current/power 是数值型",
          all(isinstance(d.get(k), (int, float)) for k in ("voltage", "current", "power")),
          f"voltage={d.get('voltage')} current={d.get('current')} power={d.get('power')}")

    ts = d.get("ts", 0)
    skew = abs(ts - time.time())
    check("ts 是真实 Unix 秒且与 PC 时钟一致（SNTP 生效）", skew < 120,
          f"ts={ts} 偏差 {skew:.0f}s" + ("（疑似退化成开机秒数）" if ts < 1_000_000_000 else ""))

    gw = wait_for(T_GW_STATUS, 0, 1.0)          # retained, 连上就该有
    if check("收到网关状态（retained）", gw is not None, T_GW_STATUS):
        g = as_json(gw)
        check("§5 字段齐全",
              all(k in g for k in ("online", "fw", "hw", "ip", "rssi", "uptime", "ts")),
              f"ip={g.get('ip')} rssi={g.get('rssi')} uptime={g.get('uptime')}s")
        check("网关状态 retain=true（§4）", gw["retain"] is True)


def send_cmd(client, payload):
    client.publish(T_CMD, json.dumps(payload, separators=(",", ":")), qos=0, retain=False)


def do_cmd(client, cmd, expect_ok, expect_code, name):
    """Send one §7 command and validate the §8 ACK."""
    since = mark()
    send_cmd(client, cmd)
    ack = wait_for(T_ACK, since, ACK_TIMEOUT)
    if not check(f"{name}：收到 ACK", ack is not None, json.dumps(cmd, separators=(",", ":"))):
        return None
    a = as_json(ack)
    check(f"{name}：ACK id 原样回传", a.get("id") == cmd.get("id"),
          f"下发 id={cmd.get('id')!r} 回执 id={a.get('id')!r}")
    check(f"{name}：result={'ok' if expect_ok else 'error'} code={expect_code}",
          a.get("result") == ("ok" if expect_ok else "error") and a.get("code") == expect_code,
          f"result={a.get('result')} code={a.get('code')} msg={a.get('msg')!r}")
    check(f"{name}：ACK retain=false（§4）", ack["retain"] is False)
    return a


def next_status(client, timeout=STATUS_TIMEOUT):
    since = mark()
    st = wait_for(T_STATUS, since, timeout)
    return as_json(st) if st else None


def step_commands(client):
    print("\n[2] §7 控制命令 -> §8 ACK -> §6 状态回读 闭环")

    do_cmd(client, {"id": 1001, "action": "start"}, True, 0, "start")
    d = next_status(client)
    if check("start 后状态回读到", d is not None):
        check("state 变为 charging", d.get("state") == "charging", str(d.get("state")))
        check("power/current 非零", (d.get("power") or 0) > 0 and (d.get("current") or 0) > 0,
              f"power={d.get('power')}kW current={d.get('current')}A voltage={d.get('voltage')}V")

    do_cmd(client, {"id": 1003, "action": "set_charge_power", "value": 60}, True, 0,
           "set_charge_power=60")
    d = next_status(client)
    if check("调功后状态回读到", d is not None):
        check("power 变为 60.0kW", abs((d.get("power") or 0) - 60.0) < 0.05, f"power={d.get('power')}")
        # 固件按 P = U x I 反推电流，60kW / 221.0V ≈ 271.5A
        expect_i = 60_000.0 / (d.get("voltage") or 221.0)
        got_i = d.get("current") or 0
        check("current 与 P=U×I 自洽", abs(got_i - expect_i) < 1.0,
              f"回读 {got_i}A，按 {d.get('voltage')}V 推算应为 {expect_i:.2f}A")

    print("\n    等待电量积分累加（充电中约 15s）...")
    e0 = (next_status(client) or {}).get("energy_charge", 0)
    time.sleep(STATUS_PERIOD * 2)
    e1 = (next_status(client) or {}).get("energy_charge", 0)
    check("energy_charge 随充电累加", e1 > e0, f"{e0} -> {e1} kWh")

    do_cmd(client, {"id": 1002, "action": "stop"}, True, 0, "stop")
    d = next_status(client)
    if check("stop 后状态回读到", d is not None):
        check("state 回到 idle", d.get("state") == "idle", str(d.get("state")))
        check("power/current 归零", d.get("power") == 0 and d.get("current") == 0,
              f"power={d.get('power')} current={d.get('current')}")


def step_error_paths(client):
    print("\n[3] 异常命令：应回 error 而不是静默丢弃或死机")

    do_cmd(client, {"id": 1004, "action": "reboot_everything"}, False, 1, "未支持的 action")
    do_cmd(client, {"id": 1005, "action": "set_charge_power", "value": 9999}, False, 2, "越界功率值")
    do_cmd(client, {"id": "abc-42", "action": "start"}, True, 0, "字符串型 id")
    # 上一条把桩开起来了，收尾停掉
    do_cmd(client, {"id": 1006, "action": "stop"}, True, 0, "收尾 stop")


def step_legacy(client):
    print("\n[4] 迁移期双协议并行：旧 /device/{id}/* 必须还活着")

    since = mark()
    st = wait_for(T_LEGACY_STATUS, since, STATUS_TIMEOUT)
    if check("旧协议状态仍在上报", st is not None, T_LEGACY_STATUS):
        d = as_json(st)
        check("旧协议 4 字段整数倍率格式不变",
              all(k in d for k in ("current_power", "current_voltage", "current_current", "onoff")),
              st["payload"])

    since = mark()
    client.publish(T_LEGACY_CTRL,
                   json.dumps({"type": "command", "name": "onoff", "value": 0, "id": "L1"},
                              separators=(",", ":")),
                   qos=0, retain=False)
    rp = wait_for(T_LEGACY_CTRL, since, ACK_TIMEOUT,
                  want=lambda m: '"reply"' in m["payload"])
    if check("旧协议命令仍能触发 reply", rp is not None):
        r = as_json(rp)
        check("旧 reply 字段正确",
              r.get("type") == "reply" and r.get("result") == "success" and r.get("id") == "L1",
              rp["payload"])

    d = next_status(client)
    if check("旧协议命令也能驱动新协议状态", d is not None):
        check("state 变为 charging（两套协议共用同一份状态）",
              d.get("state") == "charging", str(d.get("state")))

    client.publish(T_LEGACY_CTRL,
                   json.dumps({"type": "command", "name": "onoff", "value": 2, "id": "L2"},
                              separators=(",", ":")),
                   qos=0, retain=False)
    time.sleep(2)


def step_stability(client):
    print("\n[5] 稳定性：连续 6 个上报周期不许丢拍")

    since = mark()
    deadline = time.time() + STATUS_PERIOD * 6 + 6
    got = []
    while time.time() < deadline:
        with _lock:
            got = [m for m in _msgs[since:] if m["topic"] == T_STATUS]
        if len(got) >= 6:
            break
        time.sleep(0.2)

    check("30s 内收到 >=6 条设备状态", len(got) >= 6, f"实际 {len(got)} 条")
    if len(got) >= 2:
        gaps = [got[i + 1]["t"] - got[i]["t"] for i in range(len(got) - 1)]
        worst = max(gaps)
        check("上报间隔稳定在 5s 附近（无阻塞卡顿）", worst < 8.0,
              f"最大间隔 {worst:.1f}s，平均 {sum(gaps) / len(gaps):.1f}s")
        tss = [as_json(m).get("ts", 0) for m in got]
        check("ts 单调递增", all(b >= a for a, b in zip(tss, tss[1:])), f"{tss}")

    with _lock:
        events = [m for m in _msgs if m["topic"] == T_EVENT]
    check("无意外事件上报（§9 未触发过温）", not events,
          events[-1]["payload"] if events else "")


# ---------------------------------------------------------------- main
def main():
    client = make_client("eeg-loopback-test")
    client.username_pw_set(USER, PASSWD)
    client.on_message = on_message
    client.connect(BROKER, PORT, 30)
    for t in (f"{ROOT}/#", "/device/#"):
        client.subscribe(t, 0)
    client.loop_start()
    time.sleep(1.5)                              # 等 retained 消息落地

    print("=" * 68)
    print(f" EEG V1.0 闭环测试  broker={BROKER}:{PORT}  device={T_DEV}")
    print("=" * 68)

    try:
        step_schema(client)
        step_commands(client)
        step_error_paths(client)
        step_legacy(client)
        step_stability(client)
    finally:
        client.loop_stop()
        client.disconnect()

    passed = sum(1 for ok, _, _ in results if ok)
    failed = [(n, d) for ok, n, d in results if not ok]
    print("\n" + "=" * 68)
    print(f" 结果: {passed}/{len(results)} 通过")
    for n, d in failed:
        print(f"   FAIL  {n}  {d}")
    print("=" * 68)
    return 1 if failed else 0


if __name__ == "__main__":
    sys.exit(main())
