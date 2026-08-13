# MQTT 网关通信协议 V1\.0

### 1\. 文档定位

项目名称： Edge Energy Gateway（EEG）

版本： V1\.0

适用范围：

- Modbus RTU 充电桩

- 储能 PCS

- BMS

- 光伏逆变器

- 智能电表

- 环境传感器

MQTT Broker： EMQX

MQTT版本： MQTT 3\.1\.1（兼容 MQTT 5）

### 2\. 通信架构

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

- Modbus RTU

- 寄存器缓存

- JSON 编解码

- 设备模型

ESP8266 负责：

- WiFi

- MQTT

- AT 指令

- 重连

### 3\. Topic 规范

统一命名：

```Plain Text
eeg/{site}/{gateway}/{device_type}/{device_id}/{channel}
```

示例：

device\_type 固定枚举：

- gateway

- charger

- battery

- inverter

- meter

- sensor

### 4\. QoS 规范

保留消息（Retain）：

- 网关状态：Retain = true

- 设备状态：Retain = true

- 命令：Retain = false

### 5\. 网关上线协议

Topic：

```Plain Text
eeg/site01/gw001/gateway/gw001/status
```

Payload：

```Plain Text
{
  "online": true,
  "fw": "EEG-1.0.0",
  "hw": "STM32F103+ESP8266",
  "ip": "192.168.1.120",
  "rssi": -55,
  "uptime": 120,
  "ts": 1723456789
}
```

遗嘱消息：

```Plain Text
{
  "online": false,
  "ts": 1723456799
}
```

### 6\. 设备状态上报（充电桩）

Topic：

```Plain Text
eeg/site01/gw001/charger/ch001/status
```

Payload：

```Plain Text
{
  "online": true,
  "state": "charging",
  "fault_code": 0,
  "voltage": 380.1,
  "current": 31.2,
  "power": 11.8,
  "energy_charge": 12.35,
  "soc": 68,
  "temperature": 35,
  "mode": "auto",
  "ts": 1723456789
}
```

字段定义：

### 7\. 控制命令协议

Topic：

```Plain Text
eeg/site01/gw001/charger/ch001/cmd
```

### 启动

```Plain Text
{
  "id": 1001,
  "action": "start"
}
```

### 停止

```Plain Text
{
  "id": 1002,
  "action": "stop"
}
```

### 设置充电功率

```Plain Text
{
  "id": 1003,
  "action": "set_charge_power",
  "value": 60
}
```

对应寄存器：

- 1024（充电输出功率）

- 1050（启停控制）

### 8\. ACK 协议

Topic：

```Plain Text
eeg/site01/gw001/charger/ch001/ack
```

```Plain Text
{
  "id": 1001,
  "result": "ok",
  "code": 0,
  "msg": "started",
  "ts": 1723456790
}
```

### 9\. 事件协议

Topic：

```Plain Text
eeg/site01/gw001/charger/ch001/event
```

```Plain Text
{
  "level": "alarm",
  "code": 201,
  "msg": "over temperature",
  "temperature": 82,
  "ts": 1723456799
}
```



