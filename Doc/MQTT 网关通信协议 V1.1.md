# MQTT 网关通信协议 V1.1

### 1. 文档定位

项目名称： Edge Energy Gateway（EEG）

版本： V1.1

适用范围：

- Modbus RTU 充电桩
- 储能 PCS
- BMS
- 光伏逆变器
- 智能电表
- 环境传感器

MQTT Broker： EMQX

MQTT 版本： MQTT 3.1.1（兼容 MQTT 5）

约束说明：

- 设备字段、地址、倍率、枚举，统一以 `Doc/Modbus 设备模型规范 V1.1.md` 为准。
- 当前 V1 已正式定义并可直接落地的设备状态模型只有 `charger`。
- 其他 `device_type` 可以保留 Topic 命名，但在对应 Modbus 设备模型未落地前，不定义正式 Payload 字段。

### 2. 通信架构

```Plain Text
+-----------------------------+
      |         EMQX Broker           |
      +--------------+--------------+
                     |
             WiFi / MQTT
                     |
             +-------+
             |ESP8266 |
             +---+---+
                 | UART
             +---+---+
             | STM32 |
             +---+---+
                 | RS485
          Modbus RTU Master
                 |
        +--------+--------+
        |                 |
   Charger         Battery
        |                 |
      Inverter      Meter
```

STM32 负责：

- Modbus RTU 主站
- 寄存器缓存
- 轮询调度
- JSON 编解码
- 设备模型与命令执行

ESP8266 负责：

- WiFi
- MQTT
- MQTT AT 指令
- 会话建立与断线重连

### 3. Topic 规范

统一命名：

```Plain Text
eeg/{site}/{gateway}/{device_type}/{device_id}/{channel}
```

示例：

```Plain Text
eeg/site01/gw001/charger/ch001/status
```

`device_type` 固定枚举：

- `gateway`
- `charger`
- `battery`
- `inverter`
- `meter`
- `sensor`

`channel` 固定枚举：

- `status`
- `cmd`
- `ack`
- `event`

### 4. QoS 与 Retain 规范

保留消息（Retain）：

- 网关状态：`Retain = true`
- 设备状态：`Retain = true`
- 命令：`Retain = false`
- ACK：`Retain = false`
- 事件：`Retain = false`

推荐 QoS：

- 状态：`QoS 1`
- 命令：`QoS 1`
- ACK：`QoS 1`
- 事件：`QoS 1`

### 5. ESP8266 MQTT-AT 承载约束

### 5.1 结论

当前协议设计可以被 ESP8266 MQTT-AT 固件稳定承载，但必须明确采用以下规则：

- `AT+MQTTPUB` 只用于“整条 AT 命令长度小于 256 字节”的短报文。
- 设备状态等长 JSON 报文必须支持自动切换到 `AT+MQTTPUBRAW`。
- 所有 JSON 必须采用单行紧凑格式，不允许空格、缩进、换行。
- 上报字段允许新增，但不建议修改已有字段命名。

### 5.2 长度限制与工程规则

ESP8266 MQTT-AT 相关约束：

- `AT+MQTTPUB`：限制的是整条 AT 命令长度，而不是单独 payload 长度。
- `AT+MQTTPUBRAW`：以“先报长度、再发原始字节”的方式发送长报文，适合状态 JSON。
- ESP8266 默认 MQTT 缓冲区受固件内存和 `MQTT_BUFFER_SIZE_BYTE` 约束，默认常见值为 `512` 字节，因此项目侧应保留裕量。

项目统一工程规则：

- 原始 JSON payload 推荐不超过 `384` 字节。
- `AT+MQTTPUB` 的使用条件为：转义后的整条 AT 命令 `< 256` 字节。
- 一旦超过 `256` 字节，必须无条件切换 `AT+MQTTPUBRAW`，不得截断字段。
- Topic 命名保持现有规则，不通过压缩 Topic 换取可读性损失。

### 5.3 当前示例报文长度预算

以下长度为当前协议样例的工程预算值：

| 报文 | Topic 长度 | 原始 JSON 长度 | 转义后整条 AT 估算长度 | 发送方式 |
| ---- | ---- | ---- | ---- | ---- |
| 网关 `status` | `37` | `118` | `约 204` | `AT+MQTTPUB` |
| 充电桩 `status`（现有基础字段） | `37` | `171` | `约 267` | `AT+MQTTPUBRAW` |
| 充电桩 `ack` | `34` | `66` | `约 141` | `AT+MQTTPUB` |
| 充电桩 `event` | `36` | `86` | `约 163` | `AT+MQTTPUB` |

结论：

- 网关在线报文、ACK、事件报文可以继续使用 `AT+MQTTPUB`。
- 设备状态报文从 V1 起应视为“默认长报文”，规范上直接要求支持 `AT+MQTTPUBRAW`。
- 后续即使只增加少量扩展字段，设备状态报文也不应再假定可由 `AT+MQTTPUB` 可靠承载。

### 6. 网关上线协议

Topic：

```Plain Text
eeg/site01/gw001/gateway/gw001/status
```

Payload：

```Plain Text
{"online":true,"fw":"EEG-1.0.0","hw":"STM32F103+ESP8266","ip":"192.168.1.120","rssi":-55,"uptime":120,"ts":1723456789}
```

字段说明：

- `online`：在线状态
- `fw`：网关固件版本
- `hw`：硬件组合标识
- `ip`：当前 IP
- `rssi`：WiFi 信号强度
- `uptime`：运行时间，单位秒
- `ts`：Unix 时间戳，单位秒

遗嘱消息：

```Plain Text
{"online":false,"ts":0}
```

说明：

- 遗嘱消息建议通过 `AT+MQTTCONNCFG` 提前配置。
- 为避免 `AT+MQTTCONNCFG` 本身超过长度限制，遗嘱主题与遗嘱内容必须保持短小。
- `ts=0` 表示遗嘱时间由 Broker 接收时间解释，不要求由掉线瞬间实时生成。

### 7. 设备状态上报（Charger）

Topic：

```Plain Text
eeg/site01/gw001/charger/ch001/status
```

Payload：

```Plain Text
{"online":true,"state":"charging","state_code":0,"fault_code":0,"enable":1,"voltage":380.1,"current":31.2,"power":11.8,"energy_charge":12.3,"energy_discharge":0.00,"soc":68,"temperature":35,"mode":"chg","mode_code":0,"start_stop":0,"start_stop_control":1,"ts":1723456789}
```

字段定义：

| 字段 | 类型 | 来源 | 说明 |
| ---- | ---- | ---- | ---- |
| `online` | `bool` | 设备在线状态 | 基于轮询成功/失败判定 |
| `state` | `string` | 业务层派生 | 运行态，推荐值：`offline` `idle` `charging` `fault` `alarm` |
| `state_code` | `int` | `1002` | 原始 Modbus 状态码，取值见核心规范 |
| `fault_code` | `int` | `1003` | 原始故障码 |
| `enable` | `int` | `1024` | 充放电使能状态 |
| `voltage` | `number` | `1029` | `寄存器值 * 0.1V` |
| `current` | `number` | `1030` | `寄存器值 * 0.01A` |
| `power` | `number` | `1031` | `寄存器值 * 0.1kW` |
| `soc` | `int` | `1032` | 百分比 |
| `energy_charge` | `number` | `1036` | `寄存器值 * 0.1kWh` |
| `energy_discharge` | `number` | `1038` | `寄存器值 * 0.01kWh` |
| `temperature` | `int` | `1040` | `寄存器值 - 40` 后的实际温度 |
| `mode` | `string` | 业务层派生 | `chg` / `v2g` |
| `mode_code` | `int` | `1050` | 原始模式码，`0: CHG` `1: V2G` |
| `start_stop` | `int` | `1035` | 启停状态，`0: 启机` `1: 待机` `2: 停机` |
| `start_stop_control` | `int` | `1051` | 启停控制镜像，建议 `0: stop` `1: start` |
| `ts` | `int` | 网关时间 | Unix 时间戳 |

约束说明：

- `state` 为兼容既有命名保留的业务字段，不等同于 `1002` 原始值。
- `state_code`、`mode_code` 为新增字段，用于保留原始寄存器语义，避免解析歧义。
- `mode` 继续保留现有命名，但取值统一改为短字符串：`chg`、`v2g`。

### 8. 控制命令协议

Topic：

```Plain Text
eeg/site01/gw001/charger/ch001/cmd
```

命令约束：

- 下行命令 JSON 推荐长度不超过 `128` 字节。
- 命令中的寄存器地址说明统一引用 `Modbus 设备模型规范 V1.1.md` 中的“文档简写地址”。
- 真正进入 Modbus RTU 报文时，STM32 必须转换为 `PDU 原生地址 = 文档地址 - 1`。

### 启动

```Plain Text
{"id":1001,"action":"start"}
```

对应寄存器：

- `1051`（`start_stop_control`）

### 停止

```Plain Text
{"id":1002,"action":"stop"}
```

对应寄存器：

- `1051`（`start_stop_control`）

### 设置充电功率

```Plain Text
{"id":1003,"action":"set_charge_power","value":60}
```

对应寄存器：

- `1025`（`charge_power_setpoint`，单位 `0.1kW`）

### 9. ACK 协议

Topic：

```Plain Text
eeg/site01/gw001/charger/ch001/ack
```

Payload：

```Plain Text
{"id":1001,"result":"ok","code":0,"msg":"started","ts":1723456790}
```

字段说明：

- `id`：原样回传命令流水号
- `result`：`ok` / `error`
- `code`：结果码，`0` 表示成功
- `msg`：简短文本说明
- `ts`：Unix 时间戳

### 10. 事件协议

Topic：

```Plain Text
eeg/site01/gw001/charger/ch001/event
```

Payload：

```Plain Text
{"level":"alarm","code":201,"msg":"over temperature","temperature":82,"ts":1723456799}
```

字段说明：

- `level`：`info` / `warn` / `alarm`
- `code`：事件码
- `msg`：事件说明
- `temperature`：相关温度值
- `ts`：Unix 时间戳

### 11. 状态上报周期策略

轮询频率与 MQTT 上报频率分离，避免“每轮询一次就发一次 MQTT”导致串口与 Broker 压力过大。

推荐策略：

| 场景 | 设备状态上报周期 | 触发条件 |
| ---- | ---- | ---- |
| 充电中 | `1 s` | 周期上报 |
| 空闲待机 | `5 s` | 周期上报 |
| 离线 | `5 s` | 仅上报离线状态或通过遗嘱体现 |
| 故障/报警 | `立即` + `1 s` 周期 | 状态变化即刻上报 |
| 控制命令执行后 | `立即` | 写成功或失败后立即回 ACK，必要时补发新状态 |

即时上报条件：

- `state` 变化
- `fault_code` 变化
- `enable` 变化
- `mode` / `mode_code` 变化
- `start_stop` 变化

### 12. 与 Modbus 核心规范的一致性要求

- 地址解释统一采用：文档中写 `1002`，RTU 报文中发 `1001`。
- 所有倍率换算必须来源于 `Modbus 设备模型规范 V1.1.md`。
- 若 MQTT 需要增加业务友好字段，必须新增字段，不得覆盖原始字段语义。
- 若后续设备状态字段继续扩展，应优先评估对 `AT+MQTTPUBRAW` 的依赖，而不是回退修改 Topic 命名。
