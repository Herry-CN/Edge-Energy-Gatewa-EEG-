# MQTT 网关通信协议 V1.0

## 1. 文档定位

项目名称：Edge Energy Gateway（EEG）

版本：V1.0

适用范围：

- V2G 充电桩（基于 `Doc/RS485与V2G充电桩通信点表.md`）
- 后续可扩展至储能 PCS、BMS、逆变器、电表、传感器

MQTT Broker：EMQX

MQTT 版本：MQTT 3.1.1（兼容 MQTT 5）

本版本重点定义充电桩 `charger` 设备的状态上报、控制命令、回执和事件规则。字段来源与单位换算必须严格遵循 `Doc/Modbus 设备模型规范 V1.md`。

## 2. 通信架构

```Plain Text
+-----------------------------+
|         EMQX Broker         |
+--------------+--------------+
               |
         WiFi / MQTT
               |
          +----+----+
          | ESP8266 |
          +----+----+
               | UART
          +----+----+
          |  STM32  |
          +----+----+
               | RS485
       Modbus RTU Master
               |
           +---+---+
           |Charger|
           +-------+
```

STM32 负责：

- Modbus RTU 主站轮询与写寄存器
- 原始寄存器缓存
- 标准化工程量换算
- JSON 编解码
- 命令执行与 ACK 生成

ESP8266 负责：

- WiFi 入网
- MQTT 建链、订阅、发布
- AT 指令交互
- 链路重连

## 3. Topic 规范

统一命名：

```Plain Text
eeg/{site}/{gateway}/{device_type}/{device_id}/{channel}
```

约束：

- `site`：站点标识，例如 `site01`
- `gateway`：网关标识，例如 `gw001`
- `device_type`：设备类型，当前充电桩固定为 `charger`
- `device_id`：设备标识，例如 `ch001`
- `channel`：业务通道

标准通道：

- `status`
- `registers`
- `cmd`
- `ack`
- `event`

示例：

```Plain Text
eeg/site01/gw001/charger/ch001/status
eeg/site01/gw001/charger/ch001/registers
eeg/site01/gw001/charger/ch001/cmd
eeg/site01/gw001/charger/ch001/ack
eeg/site01/gw001/charger/ch001/event
```

## 4. QoS 与 Retain 规范

| Topic | QoS | Retain | 周期 |
| ----- | --- | ------ | ---- |
| 网关 `status` | 1 | true | 60 s |
| 充电桩 `status` | 1 | true | **2 s**（快通道，不含 `registers`） |
| 充电桩 `registers` | 1 | false | **10 s**（慢通道，仅原始镜像） |
| `cmd` | 1 | false | 事件 |
| `ack` | 1 | false | 事件 |
| `event` | 1 | false | 事件 |

Keepalive = 60 s。充电桩 `status` 的 2 s 周期与《STM32 软件架构设计 V1》第 5.1 节写死一致。

## 4.1 ESP8266 MQTT-AT 载荷上限（写死）

现网模块走 ESP-AT MQTT，**不能**把 800~1500 字节的「工程量 + registers」打成一条 `status`。

| 路径 | 硬限制 | 含义 |
| --- | --- | --- |
| `AT+MQTTPUB` | **整条 AT 命令 < 256 字节** | JSON 转义后更容易超，只适合 ack/event/网关心跳 |
| `AT+MQTTPUBRAW` | 本项目固件按 **payload ≤ 1024 字节** 设计 | 乐鑫 2.2.x 常见上限；超长会 `>` 后 FAIL 或模块复位 |
| STM32 拼装缓冲 | `status` ≤ **768**，`registers` ≤ **512** | 小于 PUBRAW 上限，留余量给主题与 AT 头 |

实测（紧凑 JSON、无空格换行）：

- 工程量全字段约 **1027** 字节，单独一条就会顶破 1024。
- 工程量 + `registers` 约 800~1500 字节，现网 AT **不能发**。
- 第 7.1 节白名单约 **650** 字节，落在 768 缓冲内。
- `registers` 全点表约 **403** 字节。

因此协议强制拆包，并且 **2 s 不得带全量工程量**：

1. **2 s `status`**：只允许第 7.1 节白名单（控制环 + 进线/BMS 需求），**禁止**内嵌 `registers`。紧凑 JSON 目标 **≤ 700 字节**。
2. **10 s `registers`**：Topic `eeg/{site}/{gw}/charger/{id}/registers`，仅逻辑地址键（1001 不是 41002），目标 **≤ 450 字节**。
3. 额定功率、枪组合字、时长/本次电量等静态或低频字段，云端从 `registers` 按《Modbus 设备模型规范 V1》换算，禁止再定义第三种「全量 1500 字节 status」。

实现必须走已有 `ESP8266_MQTT_Publish()`：短帧 `AT+MQTTPUB`，超 256 自动 `AT+MQTTPUBRAW`。不得为发大包改写 MQTT 接入层。

## 5. 公共负载约定

### 5.1 数值规则

- MQTT 工程量字段统一使用标准化值，不直接发送倍率缩放后的原始寄存器值。
- 原始寄存器镜像**不得**塞进 2 s 的 `status`，统一走 `registers` 慢通道。
- 未获得厂家位定义的组合寄存器，禁止拆位。进入 2 s `status` 的仅 `enable_word`、`start_stop_control_raw`；其余只出现在 `registers`。
- `registers` 的 JSON 键必须是逻辑地址字符串 `"1001"`，禁止 `"41002"`。地址换算见《Modbus 设备模型规范 V1》第 2 节：4x 真实地址 = N + 40001，报文 PDU = N。

### 5.2 枚举规则

对已知枚举字段，同时上报整数值和文本值：

- `charger_state` + `charger_state_text`
- `start_stop_state` + `start_stop_state_text`
- `work_mode` + `work_mode_text`

### 5.3 时间戳规则

- `ts`：UTC 秒级时间戳
- 若网关未完成 NTP 校时，可使用设备上电累计换算时间，但必须保持单调递增

## 6. 网关上线协议

Topic：

```Plain Text
eeg/site01/gw001/gateway/gw001/status
```

Payload：

```json
{
  "online": true,
  "fw": "EEG-1.0.0",
  "hw": "STM32F103ZE+ESP8266",
  "proto": "modbus_rtu+mqtt",
  "ip": "192.168.1.120",
  "rssi": -55,
  "uptime": 120,
  "ts": 1723456789
}
```

遗嘱消息：

```json
{
  "online": false,
  "ts": 1723456799
}
```

## 7. 充电桩状态上报协议

Topic：

```Plain Text
eeg/site01/gw001/charger/ch001/status
```

### 7.1 状态负载结构（2 s 白名单，写死）

2 s 快通道只允许下列字段。再多一个长字段名就会把紧凑 JSON 从约 650 推到 1024 附近，ESP8266 发不出去。

```json
{
  "online": true,
  "proto": "modbus_rtu",
  "slave_addr": 1,
  "ts": 1723456789,
  "charger_state": 0,
  "charger_state_text": "normal",
  "fault_code": 0,
  "start_stop_state": 1,
  "start_stop_state_text": "standby",
  "work_mode": 0,
  "work_mode_text": "chg",
  "enable_word": 3,
  "charge_power_setpoint_kw": 28.0,
  "discharge_power_setpoint_kw": 0.0,
  "output_voltage_v": 750.0,
  "output_current_a": 31.25,
  "output_power_kw": 23.4,
  "input_voltage_v": 380.1,
  "input_current_a": 31.25,
  "input_power_kw": 11.8,
  "vehicle_soc_pct": 68,
  "target_soc_pct": 80.0,
  "bms_voltage_demand_v": 760.0,
  "bms_current_demand_a": 32.5,
  "energy_charge_kwh": 12.3,
  "energy_discharge_kwh": 0.56,
  "gun_temperature_c": 35,
  "start_stop_control_raw": 0
}
```

紧凑体积约 650 字节，STM32 缓冲 768。`registers` 不在此包。

以下字段**禁止**进入 2 s `status`，只存在于原始缓存和 10 s `registers`，云端按模型规范换算：

- `capability_word`、`module_count`
- `rated_charge_power_kw`、`rated_discharge_power_kw`
- `bms_charge_power_demand_kw`、`pile_output_power_kw`
- `offline_max_discharge_power_kw`、`offline_max_charge_power_kw`
- `gun_attribute_state_word`、`gun_power_limit_raw`
- `eta_charge_complete_min`、`charge_duration_min`、`session_charge_energy_kwh`

### 7.2 字段来源约束

状态负载中的字段必须与 `Doc/Modbus 设备模型规范 V1.md` 完全一致，不得另起别名。尤其必须保持以下语义不变：

- `charger_state` 来源于寄存器 `1001`
- `start_stop_state` 来源于寄存器 `1034`
- `start_stop_control_raw` 来源于寄存器 `1050`
- `work_mode` 来源于寄存器 `1049`

### 7.3 未定义组合字处理

点表未给出位定义或工程单位时，禁止拆位。其中仅下列两项允许出现在 2 s `status`：

- `enable_word`
- `start_stop_control_raw`

下列字段只保留在原始缓存和 10 s `registers`：

- `capability_word`
- `gun_attribute_state_word`
- `gun_power_limit_raw`

### 7.4 原始寄存器慢通道

Topic：

```Plain Text
eeg/site01/gw001/charger/ch001/registers
```

周期 **10 s**，Retain = false，QoS = 1。

```json
{
  "slave_addr": 1,
  "ts": 1723456789,
  "registers": {
    "1001": 0,
    "1002": 0,
    "1003": 3,
    "1004": 12,
    "1005": 600,
    "1006": 300,
    "1007": 250,
    "1008": 248,
    "1009": 3801,
    "1010": 3125,
    "1011": 118,
    "1021": 200,
    "1022": 300,
    "1023": 3,
    "1024": 280,
    "1025": 0,
    "1026": 18,
    "1027": 300,
    "1028": 7500,
    "1029": 3125,
    "1030": 234,
    "1031": 68,
    "1032": 7600,
    "1033": 3250,
    "1034": 1,
    "1035": 123,
    "1037": 56,
    "1039": 75,
    "1040": 18,
    "1041": 42,
    "1042": 235,
    "1048": 800,
    "1049": 0,
    "1050": 0
  }
}
```

键为逻辑地址 N。厂家手册上的 41002 对应这里的 `"1001"`。

## 8. 控制命令协议

Topic：

```Plain Text
eeg/site01/gw001/charger/ch001/cmd
```

### 8.1 通用命令格式

所有命令都必须带 `id`，用于 ACK 关联。

#### 通用写寄存器命令

```json
{
  "id": 1001,
  "action": "write_register",
  "register": 1024,
  "value": 280
}
```

说明：

- `register` 必须位于白名单：`1021/1022/1023/1024/1025/1048/1049/1050`
- `value` 为待写入的原始寄存器值

### 8.2 标准化便捷命令

以下便捷命令由 STM32 负责换算成原始寄存器值，再执行 Modbus 写入。

#### 设置 EMS 离线最大放电功率

```json
{
  "id": 1101,
  "action": "set_offline_max_discharge_power",
  "value_kw": 20.0
}
```

映射：

- 寄存器：`1021`
- 原始值：`value_kw * 10`

#### 设置 EMS 离线最大充电功率

```json
{
  "id": 1102,
  "action": "set_offline_max_charge_power",
  "value_kw": 30.0
}
```

映射：

- 寄存器：`1022`
- 原始值：`value_kw * 10`

#### 设置使能字

```json
{
  "id": 1103,
  "action": "set_enable_word",
  "value": 3
}
```

映射：

- 寄存器：`1023`
- 写入值：原始 `uint16`

#### 设置充电功率

```json
{
  "id": 1104,
  "action": "set_charge_power",
  "value_kw": 28.0
}
```

映射：

- 寄存器：`1024`
- 原始值：`value_kw * 10`

#### 设置放电功率

```json
{
  "id": 1105,
  "action": "set_discharge_power",
  "value_kw": 15.0
}
```

映射：

- 寄存器：`1025`
- 原始值：`value_kw * 10`

#### 设置目标 SOC

```json
{
  "id": 1106,
  "action": "set_target_soc",
  "value_pct": 80.0
}
```

映射：

- 寄存器：`1048`
- 原始值：`value_pct * 10`

#### 设置工作模式

```json
{
  "id": 1107,
  "action": "set_mode",
  "value": "v2g"
}
```

映射：

- 寄存器：`1049`
- `chg -> 0`
- `v2g -> 1`

### 8.3 启停控制说明

寄存器 `1050` 为启停控制寄存器，但点表未给出写入值枚举，因此本协议不在标准层面强制定义 `start`/`stop` 的原始写值。

如需使用高层 `start`/`stop` 指令，必须由设备适配层先配置厂家写值映射；在该映射未确认前，云端应使用通用命令：

```json
{
  "id": 1108,
  "action": "write_register",
  "register": 1050,
  "value": 1
}
```

## 9. ACK 协议

Topic：

```Plain Text
eeg/site01/gw001/charger/ch001/ack
```

Payload：

```json
{
  "id": 1104,
  "action": "set_charge_power",
  "result": "ok",
  "code": 0,
  "msg": "write success",
  "register": 1024,
  "request_value": 28.0,
  "raw_value": 280,
  "ts": 1723456790
}
```

字段说明：

- `result`：`ok` 或 `error`
- `code`：网关错误码，`0` 表示成功
- `register`：实际写入的目标寄存器
- `request_value`：命令中使用的工程值或原始值
- `raw_value`：最终下发到 Modbus 的原始寄存器值

## 10. 事件协议

Topic：

```Plain Text
eeg/site01/gw001/charger/ch001/event
```

### 10.1 告警事件示例

```json
{
  "level": "alarm",
  "code": 201,
  "msg": "charger alarm state",
  "charger_state": 2,
  "charger_state_text": "alarm",
  "fault_code": 17,
  "ts": 1723456799
}
```

### 10.2 通信异常事件示例

```json
{
  "level": "warn",
  "code": 301,
  "msg": "modbus poll timeout",
  "slave_addr": 1,
  "register_group": "1021-1035",
  "ts": 1723456805
}
```

## 11. 一致性约束

以下规则为强制要求：

1. MQTT 字段名必须与 `Doc/Modbus 设备模型规范 V1.md` 中的标准化字段一致。
2. MQTT 不得将 `1001` 的桩健康状态误写成“charging/idle”等运行状态文本。
3. MQTT 不得将 `1050` 控制寄存器解释为 `1034` 当前状态寄存器。
4. 所有带倍率的写命令必须由 STM32 在本地完成工程值到原始值的换算。
5. 2 s 的 `status` 禁止携带 `registers`；原始镜像只允许出现在 10 s 的 `registers` 通道，且单包 ≤ 1024 字节（PUBRAW 上限）。
6. 逻辑地址与 4x 真实地址换算必须遵循《Modbus 设备模型规范 V1》第 2 节，MQTT 中只出现 N。
