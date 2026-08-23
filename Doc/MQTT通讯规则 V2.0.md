# MQTT 网关通信协议 V2.0
> 本版本仅修复重复表达与逻辑冲突,不扩展新功能。

## 1. 文档定位(收敛 Q1/Q2)
本协议定义**云端与网关之间的报文结构与交互流程**。
- 字段语义/换算:引用 Modbus 设备模型规范 V2,**协议层不重复定义、不做换算**。
- 传输链路:STM32 经 ESP8266(MQTT-AT)与云端交互。

## 2. Topic 规范(收敛 Q3)
| 方向 | Topic | 说明 |
|---|---|---|
| 上行 | device/{id}/status    | 标准化状态视图 |
| 上行 | device/{id}/registers | 原始寄存器快照 |
| 上行 | device/{id}/event     | 事件/告警 |
| 上行 | device/{id}/ack       | 命令回执 |
| 下行 | device/{id}/cmd       | 命令下发 |

## 3. 通用报文规范
- 编码:UTF-8 JSON
- 必含:`msg_id`、`ts`(UTC ms)、`type`
- 载荷:遵循 ESP8266 AT 单帧长度限制,超限需分片(不在本版扩展分片新协议)

## 4. 报文结构
### 4.1 status(标准化视图)
{
  "type": "status",
  "msg_id": "uuid",
  "ts": 1690000000000,
  "data": {
    "state_enum": 0,
    "soc": 55.0,
    "charge_power": 7.0,
    "discharge_power": 0.0,
    "mode": "v2g"
  }
}
> 字段单位/语义均引用设备模型规范 V2,协议层不另行说明(Q1)。

### 4.2 registers(原始快照)
{
  "type": "registers",
  "msg_id": "uuid",
  "ts": 1690000000000,
  "data": { "1001": 0, "1034": 0, "1050": 0 }
}
> 上报原始值,不做换算(Q2)。

### 4.3 cmd(下行命令)
{
  "type": "cmd",
  "msg_id": "uuid",
  "ts": 1690000000000,
  "cmd": "set_charge_power",
  "value": 7.0
}
> value 为工程值;换算规则以设备模型规范 V2 §2 为准(Q2)。

### 4.4 ack(回执)
{
  "type": "ack",
  "msg_id": "uuid",
  "ref_id": "对应 cmd 的 msg_id",
  "result": "ok",
  "reason": ""
}
### 4.5 event(事件)
{
  "type": "event",
  "msg_id": "uuid",
  "ts": 1690000000000,
  "level": "fault",
  "code": "poll_timeout"
}
## 5. 上报周期(统一 Q4)
| 报文 | 周期 |
|---|---|
| status    | 2s |
| registers | 10s |
| event     | 触发即发(超时 / fault·alarm / 写失败) |
| ack       | 命令处理完成即发 |

## 6. 命令白名单(引用,不重复换算)
set_offline_max_discharge_power / set_offline_max_charge_power /
set_charge_power / set_discharge_power / set_target_soc / set_mode /
(1050 raw 控制命令:需厂家确认后开放)
> 各命令对应寄存器与换算见设备模型规范 V2。

## 7. 冲突约束
- 协议层禁止自定义字段换算(一律引用设备模型规范)
- registers 报文只传原始值,status 报文只传标准化工程值
- 命令 value 语义以工程值为准,不得混入原始值
