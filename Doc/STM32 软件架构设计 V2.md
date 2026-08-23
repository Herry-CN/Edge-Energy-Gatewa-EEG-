# STM32 软件架构设计 V2
> 变更说明:本版本仅修复 V1 的重复表达与逻辑冲突,不扩展新功能。

## 1. 文档定位与协同原则(统一收敛 P3)
本文档定义 STM32 的软件分层、模块职责、数据流与命令流。
外部依赖遵循**单一事实来源(SSOT)**原则:

| 内容 | 权威来源 |
|---|---|
| 字段定义 / 寄存器语义 | Modbus 设备模型规范 V1 |
| 云端报文结构 | MQTT 网关通信协议 V1.0 |
| 寄存器地址 / 点表 | RS485与V2G充电桩通信点表 |

> 本文档不重复定义上述内容,仅引用。

落地场景:V2G 充电桩 + RS485(Modbus RTU) + ESP8266(MQTT-AT)。

## 2. 总体架构

```Plain Text
+--------------------------------------------------------------+
|                          Application                         |
+-------------------------+------------------------------------+
| Device Manager          | MQTT Command Manager               |
+-------------------------+------------------------------------+
| Charger Device          | JSON Codec                         |
+-------------------------+------------------------------------+
| Register Cache          | ESP8266 AT Driver                  |
+-------------------------+------------------------------------+
| Modbus Master           | UART WiFi (USART3)                |
+-------------------------+------------------------------------+
| UART RS485 (USART2)     | Debug UART (USART1)              |
+--------------------------------------------------------------+
```

**设计原则(去重后保留三条核心):**
1. STM32 不实现原生 MQTT,仅通过 ESP8266 MQTT-AT 交互。
2. STM32 完整维护原始寄存器缓存,基于缓存生成标准化状态视图。
3. 语义与命名统一以设备模型规范为准(见 §1)。

## 3. 硬件资源分工
| 外设 | 用途 |
|---|---|
| USART1 | 调试串口 |
| USART2 | RS485 / Modbus RTU 主站(半双工,MAX485,需 DE/RE 控制) |
| USART3 | ESP8266 AT 通信 |

BSP 必须支持:USART2 TX/RX、DE/RE 方向控制、接收超时与帧边界处理。

## 4. 模块划分
### 4.1 BSP
仅处理硬件外设(UART/GPIO/Timer/Watchdog),不承载业务字段解释。
RS485 方向切换、收发、中断缓冲属于 BSP。

### 4.2 Modbus Master
文件:modbus_master.c / modbus_crc.c
职责:组帧解帧、CRC16、读保持寄存器、写单寄存器、超时与异常码处理。

```c
int mb_read(uint8_t addr, uint16_t start_reg, uint16_t num, uint16_t *buf);
int mb_write(uint8_t addr, uint16_t reg, uint16_t value);
```

说明：
- mb_read:返回通信结果,数据写入 buf
- mb_write:仅允许对白名单寄存器写操作

### 4.3 Register Cache
缓存点表全部有效寄存器原始值,标记新鲜度/有效性/更新时间,
为 Charger Device 提供统一读接口。

### 4.4 Charger Device
原始寄存器 ↔ 标准化工程量映射;命令合法性校验与换算。

### 4.5 JSON Codec / MQTT Command Manager / Device Manager
遵循 MQTT 协议字段规范与载荷限制,负责编解码、命令分发与设备协调。

## 5. 数据流与命令流(合并 P4)
### 5.1 周期上报
- mqtt_status_task(每 2s)   -> build_status    -> publish(status)
- mqtt_registers_task(每10s) -> build_registers -> publish(registers)

### 5.2 命令下发
MQTT cmd -> AT Driver -> Command Manager -> JSON parse ->
Device Manager -> Charger Device -> 合法性校验 ->
工程值→原始值换算 -> mb_write -> 回读/缓存更新 ->
build ack -> publish(ack)

### 5.3 事件上报
触发:轮询超时 / 状态进入 fault·alarm / 写寄存器失败
流程:error detect -> event build -> publish(event)

## 6. 命令换算表(统一唯一来源,消除 P4)
| 命令 | 寄存器 | 换算 |
|---|---|---|
| set_offline_max_discharge_power | 1021 | kW × 10 |
| set_offline_max_charge_power    | 1022 | kW × 10 |
| set_charge_power                | 1024 | kW × 10 |
| set_discharge_power             | 1025 | kW × 10 |
| set_target_soc                  | 1048 | % × 10  |
| set_mode                        | 1049 | chg/v2g 枚举 |

## 7. 寄存器语义边界(核心修复 P1/P2/P5)
> 本章专门消除 V1 中的语义混用冲突。

1. **1050 = 控制寄存器**:点表未定义具体写值。
   - 默认仅支持 `write_register(1050, raw_value)`。
   - 如需 start/stop 高层命令,必须在 charger_device 中
     显式配置**厂家确认的写值映射**。
2. **1034 = 状态枚举**,与 1050 是**两个独立寄存器**,严禁互相替代。
3. **1001** 的语义以设备模型规范为准,**禁止**在架构层解释为
   "charging/idle" 运行状态。

## 8. 点表一致性约束(合并 P6)
### 8.1 必须完整覆盖的有效地址
1001-1011、1021-1035、1037、1039-1042、1048-1050

### 8.2 禁止冲突项(硬约束)
- 禁止将 1001 解释为运行状态
- 禁止将 1034 与 1050 视为同一寄存器
- 禁止将 1023 / 1026 组合字拆为未经厂家确认的布尔字段
- 禁止将 1027 按未确认单位上报为功率工程值

## 9. 文档变更顺序(不变)
点表变更时按序更新:
RS485点表 -> Modbus设备模型规范 -> STM32架构设计 -> MQTT通信协议
