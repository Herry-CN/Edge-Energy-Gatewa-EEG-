"""Expected MQTT status/registers from mbserver holding values.

mbserver 4xxxxx = 400001 + N, so 401010 is logic address 1009.
STM32 PDU = N. Scaling matches Doc/Modbus 设备模型规范 V1.md.
"""
from __future__ import print_function


def fmt_scaled(raw, dec):
    raw = int(raw)
    if dec == 2:
        return "%d.%02d" % (raw // 100, raw % 100)
    return "%d.%d" % (raw // 10, raw % 10)


def status_from_regs(r):
    ctrl = int(r.get(1050, 0))
    pile = int(r.get(1001, 0))
    fault = int(r.get(1002, 0))
    if pile == 1:
        state = "fault"
    elif pile == 2:
        state = "alarm"
    else:
        state = "normal"
    mode = int(r.get(1049, 0))
    return {
        "state": state,
        "state_code": pile,
        "fault_code": fault,
        "enable": int(r.get(1023, 0)),
        "voltage": fmt_scaled(r.get(1028, 0), 1),
        "current": fmt_scaled(r.get(1029, 0), 2),
        "power": fmt_scaled(r.get(1030, 0), 1),
        "input_voltage": fmt_scaled(r.get(1009, 0), 1),
        "input_current": fmt_scaled(r.get(1010, 0), 2),
        "input_power": fmt_scaled(r.get(1011, 0), 1),
        "charge_power": fmt_scaled(r.get(1024, 0), 1),
        "energy_charge": fmt_scaled(r.get(1035, 0), 1),
        "energy_discharge": fmt_scaled(r.get(1037, 0), 2),
        "soc": int(r.get(1031, 0)),
        "temperature": int(r.get(1039, 0)) - 40,
        "mode": "v2g" if mode == 1 else "chg",
        "mode_code": mode,
        "start_stop": int(r.get(1034, 0)),
        "start_stop_control": ctrl,
    }


# Screenshot: mbserver first, COM7, values at 401010/011/012/025/051
MBSERVER = {
    1009: 230,   # 进线电压  401010
    1010: 32,    # 进线电流  401011
    1011: 7,     # 进线功率  401012
    1024: 100,   # 充电功率设定 401025
    1028: 0,     # 输出电压  401029
    1029: 0,     # 输出电流  401030
    1030: 0,     # 输出功率  401031
    1031: 0,     # SOC       401032
    1039: 0,     # 枪温 raw  401040 → -40
    1050: 1,     # 启停控制  401051
}


def test_mbserver_screenshot():
    got = status_from_regs(MBSERVER)
    assert got["input_voltage"] == "23.0", got
    assert got["input_current"] == "0.32", got
    assert got["input_power"] == "0.7", got
    assert got["charge_power"] == "10.0", got
    assert got["voltage"] == "0.0", got
    assert got["current"] == "0.00", got
    assert got["power"] == "0.0", got
    assert got["temperature"] == -40, got
    assert got["start_stop_control"] == 1, got
    assert got["state"] == "normal", got
    assert got["state_code"] == 0, got
    assert got["soc"] == 0, got


# 计划附图：mbserver 现值（401029=4000 等）→ 烧录单点轮询后 MQTT 应报这些工程量
MBSERVER_LIVE = {
    1001: 0, 1002: 0, 1003: 257, 1004: 4,
    1009: 3800, 1010: 6500, 1011: 405,
    1023: 257, 1024: 400,
    1028: 4000, 1029: 9600, 1030: 384,
    1031: 62, 1034: 0,
    1035: 57920, 1037: 56789, 1039: 75,
    1049: 0, 1050: 0,
}


def test_mbserver_live_screenshot():
    got = status_from_regs(MBSERVER_LIVE)
    assert got["state"] == "normal", got
    assert got["state_code"] == 0, got
    assert got["fault_code"] == 0, got
    assert got["enable"] == 257, got
    assert got["voltage"] == "400.0", got
    assert got["current"] == "96.00", got
    assert got["power"] == "38.4", got
    assert got["soc"] == 62, got
    assert got["energy_charge"] == "5792.0", got
    assert got["energy_discharge"] == "567.89", got
    assert got["temperature"] == 35, got
    assert got["mode"] == "chg", got
    assert got["mode_code"] == 0, got
    assert got["start_stop"] == 0, got
    assert got["start_stop_control"] == 0, got
    assert got["input_voltage"] == "380.0", got
    assert got["input_current"] == "65.00", got


def test_pile_status_1001():
    assert status_from_regs({1001: 0})["state"] == "normal"
    assert status_from_regs({1001: 1})["state"] == "fault"
    assert status_from_regs({1001: 2})["state"] == "alarm"
    # 1050 不影响 1001 语义
    got = status_from_regs({1001: 0, 1050: 1})
    assert got["state"] == "normal"
    assert got["start_stop_control"] == 1
    got = status_from_regs({1009: 2300})
    assert got["input_voltage"] == "230.0", got


if __name__ == "__main__":
    test_mbserver_screenshot()
    test_mbserver_live_screenshot()
    test_pile_status_1001()
    print("ok  expected status for current mbserver:")
    d = status_from_regs(MBSERVER)
    for k in sorted(d):
        print("  %s = %s" % (k, d[k]))
