# STM32 软件架构设计 V1.1

### 1. 总体架构

```Plain Text
+-----------------------------------------------------------+
|                        Application                        |
+-------------------------+---------------------------------+
| Device Manager          | MQTT Command Manager            |
+-------------------------+---------------------------------+
| Poll Scheduler          | JSON Codec                      |
+-------------------------+---------------------------------+
| Register Cache          | MQTT Publish Adapter            |
+-------------------------+---------------------------------+
| Modbus Master           | ESP8266 AT Driver              |
+-------------------------+---------------------------------+
| UART RS485              | UART WiFi                      |
+-----------------------------------------------------------+
```

### 2. 设计原则

- 设备数据源唯一来自 `Doc/Modbus 设备模型规范 V1.1.md`。
- STM32 负责设备侧采集、缓存、调度、命令执行与 JSON 组包。
- ESP8266 不参与业务解析，只负责 WiFi、MQTT、AT 命令收发与连接维护。
- RS485 轮询必须采用“单帧调度”，避免串口与 AT 命令互相阻塞。
- MQTT 上报与 Modbus 轮询解耦，不允许“每轮询一帧就立即发布一次 MQTT”。

### 3. 模块划分

### BSP

负责：

- UART1 / UART3（ESP8266，按工程实际口线配置）
- UART2（RS485）
- GPIO
- Timer
- Watchdog

### Modbus

文件：

```Plain Text
modbus_master.c
modbus_crc.c
```

接口：

```Plain Text
int mb_read (uint8_t addr, uint16_t reg, uint16_t num, uint16_t *dst);
int mb_write(uint8_t addr, uint16_t reg, uint16_t value);
```

地址规则：

```Plain Text
文档简写地址 = 1002
完整显示地址 = 401002
PDU原生地址 = 1001
PDU原生地址 = 文档简写地址 - 1
```

统一要求：

- 驱动接口 `mb_read()` / `mb_write()` 的 `reg` 参数统一传 `PDU 原生地址`。
- 文档和对外协议中仍保留 `1002` 这类“文档简写地址”。
- 若调试工具使用 `4xxxxxx` 地址风格，则显示为 `401002`。

### Register Cache

建议结构：

```Plain Text
typedef struct
{
    uint16_t state_code;             // 1002, 原始状态码
    uint16_t fault_code;             // 1003
    uint16_t enable;                 // 1024
    uint16_t charge_power_setpoint;  // 1025
    uint16_t voltage;                // 1029, 0.1V
    uint16_t current;                // 1030, 0.01A
    uint16_t power;                  // 1031, 0.1kW
    uint16_t soc;                    // 1032, %
    uint16_t energy_charge;          // 1036, 0.1kWh
    uint16_t energy_discharge;       // 1038, 0.01kWh
    uint16_t temperature_raw;        // 1040, 实际温度 = raw - 40
    uint16_t mode_code;              // 1050
    uint16_t start_stop;             // 1035, 启停状态
    uint16_t start_stop_control;     // 1051, 启停控制
    uint8_t  online;                 // 业务在线状态
    uint8_t  fail_count;             // 连续失败次数
    uint32_t last_ok_ms;             // 最近一次成功轮询时间
} ChargerRegCache;
```

派生规则：

- `temperature = temperature_raw - 40`
- `state_code` 保存原始寄存器值，不直接覆盖为 `charging`、`idle`
- MQTT 所需的 `state`、`mode` 等字符串由业务层派生生成

### Device Layer

目录：

```Plain Text
devices/
    charger.c
    battery.c
    inverter.c
```

统一接口：

```Plain Text
void device_init(void);
void device_poll(void);
void device_build_status_json(char *buf, uint16_t buf_sz);
int  device_execute_command(Command *cmd);
```

当前 V1 实施约束：

- `charger` 已定义完整寄存器映射，可直接启用。
- `battery`、`inverter` 等目录可以保留，但在对应设备模型文档未明确前，不生成正式状态 JSON。

### MQTT Layer

STM32 不实现完整 MQTT 协议栈，只维护：

```Plain Text
mqtt_publish(topic, payload);
mqtt_subscribe(topic);
mqtt_process_rx();
```

底层通过：

```Plain Text
AT+MQTTPUB=...
AT+MQTTPUBRAW=...
AT+MQTTSUB=...
```

发送适配规则：

- 短报文走 `AT+MQTTPUB`
- 长报文走 `AT+MQTTPUBRAW`
- 判定条件不是 payload 原始长度，而是“转义后的整条 AT 命令长度是否小于 256 字节”
- JSON 一律使用紧凑格式，不允许格式化换行

### JSON Codec

建议使用：

- 轻量级手写编码
- 或 `cJSON`（仅在 Flash / RAM 预算允许时）

负责：

- `build_gateway_status_json()`
- `build_device_status_json()`
- `build_ack_json()`
- `build_event_json()`
- `parse_cmd_json()`

### 4. 轮询调度架构

### 4.1 目标

轮询调度需要同时满足：

- 安全状态变化能快速被发现
- 电参数据有足够实时性
- 总线不会因连续批量读而堵塞 MQTT AT
- 写命令后能快速完成结果确认

### 4.2 分级策略

| 等级 | 字段 | 推荐周期 |
| ---- | ---- | ---- |
| A：安全/控制类 | `state_code` `fault_code` `enable` `mode_code` `start_stop` | `300 ms ~ 500 ms` |
| B：运行电参类 | `voltage` `current` `power` `soc` `temperature_raw` | `1000 ms`（运行中） / `3000 ms`（空闲） |
| C：累计量类 | `energy_charge` `energy_discharge` | `5000 ms` |

### 4.3 推荐读块

```Plain Text
Block-A1 : 1002~1003  -> state_code, fault_code
Block-A2 : 1024~1025  -> enable, charge_power_setpoint
Block-B1 : 1029~1032  -> voltage, current, power, soc
Block-A3 : 1035       -> start_stop
Block-C1 : 1036       -> energy_charge
Block-C2 : 1038       -> energy_discharge
Block-B2 : 1040       -> temperature_raw
Block-A4 : 1050~1051  -> mode_code, start_stop_control
```

实际发报文时统一换算为 PDU 原生地址：

```Plain Text
1002 -> 1001
1024 -> 1023
1029 -> 1028
1035 -> 1034
1036 -> 1035
1038 -> 1037
1040 -> 1039
1050 -> 1049
1051 -> 1050
```

### 4.4 调度方式

建议实现为“定时器 + 分级轮询 + 单帧执行”：

```Plain Text
1. 主循环每次只允许发送 1 帧 Modbus 请求
2. 先检查是否存在命令回读任务
3. 再检查 A 类是否到期，并在同级块之间轮询
4. 再检查 B 类是否到期，并在同级块之间轮询
5. 最后检查 C 类是否到期，并在同级块之间轮询
```

伪代码：

```c
void device_poll(void)
{
    if (!bus_idle()) return;

    if (verify_job_due()) {
        poll_verify_block();
        return;
    }

    if (group_a_due()) {
        poll_group_a_next();
        return;
    }

    if (group_b_due()) {
        poll_group_b_next();
        return;
    }

    if (group_c_due()) {
        poll_group_c_next();
        return;
    }
}
```

### 4.5 离线判定

推荐策略：

- 连续 `3` 次轮询失败：置 `online = 0`
- 离线后仅保留 `3000 ms` 一次的轻量探测
- 连续 `2` 次探测成功：恢复在线

### 4.6 写命令后的处理

对于以下寄存器写操作：

- `1024` `enable`
- `1025` `charge_power_setpoint`
- `1051` `start_stop_control`

执行顺序建议为：

```Plain Text
接收 MQTT 命令
-> 解析命令
-> 发送 Modbus 写请求
-> 写成功后立刻安排 300 ms 内的回读校验
-> 生成 ACK
-> 由下一次状态上报输出最新状态
```

说明：

- ACK 关注的是“命令是否执行成功”
- `status` 关注的是“设备当前状态是什么”
- 两者不能相互替代

### 5. MQTT 上报策略

推荐策略：

| 报文 | 周期 / 时机 |
| ---- | ---- |
| 网关状态 | 上线立即发送，之后每 `30 s` 发送一次 |
| 设备状态（充电中） | 每 `1 s` 发送一次 |
| 设备状态（空闲） | 每 `5 s` 发送一次 |
| 设备状态（故障/报警变化） | 立即发送 |
| ACK | 命令执行完成立即发送 |
| Event | 事件触发立即发送 |

即时触发条件：

- `state` 变化
- `fault_code` 变化
- `enable` 变化
- `mode_code` 变化
- `start_stop` 变化

### 6. 业务状态派生规则

为兼容现有 MQTT 字段命名，建议从寄存器缓存派生以下字段：

```Plain Text
state_code -> 原始寄存器值
state      -> offline / idle / charging / fault / alarm
mode_code  -> 原始寄存器值
mode       -> chg / v2g
```

推荐优先级：

```Plain Text
if offline          -> state = "offline"
else if state_code==1 -> state = "fault"
else if state_code==2 -> state = "alarm"
else if start_stop==1 or current > threshold -> state = "charging"
else -> state = "idle"
```

### 7. 直接落地要求

- 所有寄存器地址在代码中必须显式区分“文档地址”和“PDU 地址”
- 所有 MQTT 状态 JSON 必须可追溯到寄存器缓存
- 设备状态长报文必须允许走 `AT+MQTTPUBRAW`
- 新接入设备前，必须先更新 `Modbus 设备模型规范 V1.1.md`，再更新驱动和 MQTT 协议层
