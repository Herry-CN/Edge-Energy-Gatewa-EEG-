# STM32 软件架构设计 V1

## 1. 文档定位

本文档定义 STM32 在本项目中的软件分层、模块职责、数据流、命令流与缓存结构。其外部接口必须与以下文档保持一致：

- `Doc/Modbus 设备模型规范 V1.md`
- `Doc/MQTT 网关通信协议 V1.0.md`
- `Doc/RS485与V2G充电桩通信点表.md`

当前版本以 `V2G 充电桩 + RS485(Modbus RTU) + ESP8266(MQTT-AT)` 为唯一落地场景。

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

设计原则：

1. `STM32` 不实现原生 MQTT 协议栈，只通过 `ESP8266 MQTT-AT` 与 Broker 交互。
2. `STM32` 必须完整维护充电桩原始寄存器缓存，并基于缓存生成标准化状态视图。
3. 设备模型、MQTT 字段命名、寄存器语义均以 `Modbus 设备模型规范 V1` 为准。

## 3. 硬件资源分工

### 3.1 UART 资源

- `USART1`：调试串口
- `USART2`：RS485 / Modbus RTU 主站
- `USART3`：ESP8266 AT 通信

### 3.2 RS485 资源

- 通信协议：Modbus RTU
- 总线形态：半双工 RS485
- 收发器：MAX485 或等效芯片
- BSP 必须支持：
  - `USART2 TX/RX`
  - `DE/RE` 方向控制 GPIO
  - 接收超时与帧边界处理

## 4. 模块划分

### 4.1 BSP

负责：

- `USART1` 调试输出
- `USART2` RS485 驱动
- `USART3` ESP8266 AT 串口
- GPIO
- Timer / SysTick
- Watchdog

边界：

- BSP 只处理硬件外设，不承载业务字段解释。
- RS485 的方向切换、串口收发、中断缓冲属于 BSP。

### 4.2 Modbus Master

文件：

```Plain Text
modbus_master.c
modbus_crc.c
```

职责：

- 组帧 / 解帧
- CRC16 校验
- 读保持寄存器
- 写单个保持寄存器
- 超时、异常码处理

推荐接口：

```c
int mb_read(uint8_t addr, uint16_t start_reg, uint16_t num, uint16_t *buf);
int mb_write(uint8_t addr, uint16_t reg, uint16_t value);
```

说明：

- `mb_read` 返回通信结果，寄存器数据写入 `buf`
- `mb_write` 只允许对白名单寄存器执行写操作

### 4.3 Register Cache

职责：

- 缓存点表中全部有效寄存器原始值
- 标记数据新鲜度、有效性、最近更新时间
- 为 `Charger Device` 提供统一读接口

原始缓存结构建议如下：

```c
typedef struct
{
    uint16_t reg_1001_charger_state;
    uint16_t reg_1002_fault_code;
    uint16_t reg_1003_capability_word;
    uint16_t reg_1004_module_count;
    uint16_t reg_1005_rated_charge_power;
    uint16_t reg_1006_rated_discharge_power;
    uint16_t reg_1007_bms_charge_power_demand;
    uint16_t reg_1008_pile_output_power;
    uint16_t reg_1009_input_voltage;
    uint16_t reg_1010_input_current;
    uint16_t reg_1011_input_power;
    uint16_t reg_1021_offline_max_discharge_power;
    uint16_t reg_1022_offline_max_charge_power;
    uint16_t reg_1023_enable_word;
    uint16_t reg_1024_charge_power_setpoint;
    uint16_t reg_1025_discharge_power_setpoint;
    uint16_t reg_1026_gun_attribute_state_word;
    uint16_t reg_1027_gun_power_limit_raw;
    uint16_t reg_1028_output_voltage;
    uint16_t reg_1029_output_current;
    uint16_t reg_1030_output_power;
    uint16_t reg_1031_vehicle_soc;
    uint16_t reg_1032_bms_voltage_demand;
    uint16_t reg_1033_bms_current_demand;
    uint16_t reg_1034_start_stop_state;
    uint16_t reg_1035_energy_charge;
    uint16_t reg_1037_energy_discharge;
    uint16_t reg_1039_gun_temperature;
    uint16_t reg_1040_eta_charge_complete_min;
    uint16_t reg_1041_charge_duration_min;
    uint16_t reg_1042_session_charge_energy;
    uint16_t reg_1048_target_soc;
    uint16_t reg_1049_work_mode;
    uint16_t reg_1050_start_stop_control_raw;
    uint32_t ts;
    uint8_t valid;
} ChargerRegisterCache;
```

### 4.4 Charger Device

职责：

- 将原始寄存器缓存映射为标准化工程量
- 执行点表约束和枚举解释
- 对 MQTT 命令执行写寄存器转换
- 统一管理充电桩对象生命周期

标准化视图建议如下：

```c
typedef struct
{
    uint16_t charger_state;
    uint16_t fault_code;
    uint16_t capability_word;
    uint16_t module_count;
    float rated_charge_power_kw;
    float rated_discharge_power_kw;
    float bms_charge_power_demand_kw;
    float pile_output_power_kw;
    float input_voltage_v;
    float input_current_a;
    float input_power_kw;
    float offline_max_discharge_power_kw;
    float offline_max_charge_power_kw;
    uint16_t enable_word;
    float charge_power_setpoint_kw;
    float discharge_power_setpoint_kw;
    uint16_t gun_attribute_state_word;
    uint16_t gun_power_limit_raw;
    float output_voltage_v;
    float output_current_a;
    float output_power_kw;
    uint16_t vehicle_soc_pct;
    float bms_voltage_demand_v;
    float bms_current_demand_a;
    uint16_t start_stop_state;
    float energy_charge_kwh;
    float energy_discharge_kwh;
    int16_t gun_temperature_c;
    uint16_t eta_charge_complete_min;
    uint16_t charge_duration_min;
    float session_charge_energy_kwh;
    float target_soc_pct;
    uint16_t work_mode;
    uint16_t start_stop_control_raw;
} ChargerStatusView;
```

规则：

- `1039` 温度字段采用 `raw - 40`
- `1001`、`1034`、`1049` 必须保留原始枚举值，并在 JSON 输出时附带文本值
- `1023`、`1026`、`1027`、`1050` 不得在设备层擅自拆位

### 4.5 Device Manager

职责：

- 管理一个或多个设备对象
- 调度 `charger_device_poll`
- 统一处理命令执行入口
- 对外提供状态组装接口

统一接口建议：

```c
void device_init(void);
void device_poll(void);
int device_build_status_json(char *buf, uint16_t size);      /* <= 768，仅 7.1 白名单 */
int device_build_registers_json(char *buf, uint16_t size);   /* <= 512 */
int device_execute_command(const Command *cmd);
```

### 4.6 JSON Codec

负责：

- 将 `ChargerStatusView` 组装成 2 s MQTT `status` 负载（不含 `registers`）
- 将 `ChargerRegisterCache` 组装成 10 s MQTT `registers` 负载
- 解析 MQTT `cmd` JSON
- 生成 `ack` / `event` JSON

要求：

- 字段名必须完全匹配 `MQTT 网关通信协议 V1.0`
- 必须同时输出标准化工程量字段和枚举文本字段
- 2 s `status` 只输出《MQTT 网关通信协议 V1.0》第 7.1 节白名单，不得输出 `registers`，缓冲 ≤ 768
- 10 s 另发 `registers` 慢通道，键为逻辑地址 N，缓冲 ≤ 512

### 4.7 MQTT Command Manager

职责：

- 接收 ESP8266 下发的 MQTT 消息
- 解析 `cmd` Topic
- 转换为内部 `Command`
- 调用 `device_execute_command`
- 发送 `ack`

命令白名单必须与点表一致：

- `1021`
- `1022`
- `1023`
- `1024`
- `1025`
- `1048`
- `1049`
- `1050`

### 4.8 ESP8266 AT Driver

职责：

- WiFi 入网
- MQTT 参数配置
- Broker 连接 / 订阅 / 发布
- 断链恢复

边界：

- 不处理充电桩寄存器语义
- 不处理 JSON 字段定义
- 只作为传输层
- 发布必须走 `ESP8266_MQTT_Publish()`：AT 命令 <256 用 `AT+MQTTPUB`，否则 `AT+MQTTPUBRAW`；payload 上限 1024，`status` 实现上限 768

## 5. 轮询与交互流程

### 5.1 时隙与周期（写死）

RS485 为半双工 9600 8-N-1，一帧往返约 40–80 ms。调度以 **100 ms 时隙** 为节拍，**每个时隙最多 1 帧**（含命令写）。

| 任务 | 周期 | 时隙占用 | 说明 |
| --- | --- | --- | --- |
| G1 `1001-1011` | **200 ms** | 约 5 帧/s | 桩健康、进线。**不能** 100 ms：10 帧/s 会占满总线，G2/G3/G4 饿死 |
| G2 `1021-1035` | **500 ms** | 2 帧/s | 输出、设定回读、SOC |
| G3 `1037-1042` | **1000 ms** | 1 帧/s | 电量、温度、统计 |
| G4 `1048-1050` | **1000 ms** | 1 帧/s | 模式、目标 SOC、1050 |
| MQTT `status` | **2000 ms** | 0 帧 | 只读缓存，USART3 AT，不占 RS485 |
| MQTT `registers` | **10000 ms** | 0 帧 | 慢通道，逻辑地址键 |
| `mb_write` | 命令到达立即 | 抢占下一时隙 | 优先于 G1–G4 |

1 秒内预算：G1×5 + G2×2 + G3×1 + G4×1 = 9 帧，低于 10 个时隙，剩 1 个时隙给写命令。

到期调度：每个时隙取「已到期且等待最久」的一组；写命令优先级最高。禁止在一个 `device_poll` 里连发 G1+G2+G3+G4。

```Plain Text
every 100ms slot:
  if pending write -> mb_write; return
  if G1 due (200ms) -> mb_read G1
  else if G2 due (500ms) -> mb_read G2
  else if G3 due (1000ms) -> mb_read G3
  else if G4 due (1000ms) -> mb_read G4
  register_cache_update for that group only

every 2000ms (USART3, not RS485):
  json_codec_build_status   /* no registers object */
  esp8266_publish(status)   /* AT+MQTTPUB or PUBRAW, payload <= 768 */

every 10000ms:
  esp8266_publish(registers) /* payload <= 512 */
```

逻辑地址与 PDU：组帧使用 `MB_PDU_FROM_LOGIC(n)`，默认 PDU=N；4x 真实地址 = N+40001，见 Modbus 规范第 2 节。

### 5.2 状态轮询流程

```Plain Text
device_poll  (每 100ms 进入，每次最多 1 次 mb_read)
  -> charger_device_poll
      -> 按 5.1 选择 G1/G2/G3/G4 之一
      -> mb_read
      -> register_cache_update
      -> 若该组影响视图则刷新 ChargerStatusView

mqtt_status_task (每 2s)
  -> json_codec_build_status
  -> esp8266_publish(status)

mqtt_registers_task (每 10s)
  -> json_codec_build_registers
  -> esp8266_publish(registers)
```

### 5.3 命令下发流程

```Plain Text
MQTT cmd
  -> ESP8266 AT Driver
  -> MQTT Command Manager
  -> JSON Codec parse
  -> Device Manager
  -> Charger Device
  -> 参数合法性校验
  -> 工程值 -> 原始寄存器值换算
  -> mb_write
  -> 结果回读/缓存更新
  -> JSON Codec build ack
  -> ESP8266 publish(ack)
```

### 5.4 事件上报流程

触发条件示例：

- Modbus 轮询超时
- 充电桩状态从 `normal` 进入 `fault/alarm`
- 写寄存器失败

流程：

```Plain Text
error detect
  -> event build
  -> esp8266_publish(event)
```

## 6. 命令语义约束

### 6.1 工程值换算

以下命令必须在 STM32 本地完成换算：

- `set_offline_max_discharge_power` -> `1021` -> `kW * 10`
- `set_offline_max_charge_power` -> `1022` -> `kW * 10`
- `set_charge_power` -> `1024` -> `kW * 10`
- `set_discharge_power` -> `1025` -> `kW * 10`
- `set_target_soc` -> `1048` -> `% * 10`
- `set_mode` -> `1049` -> `chg/v2g` 枚举

### 6.2 启停控制

`1050` 为控制寄存器，但点表未定义具体写值，因此架构层必须遵循以下规则：

- 默认仅支持 `write_register(1050, raw_value)` 形式
- 如需暴露 `start/stop` 高层命令，必须在 `charger_device` 中显式配置厂家写值映射
- 严禁直接用 `1034` 的状态枚举值替代 `1050` 的控制写值

## 7. 与点表的一致性要求

### 7.1 必须完整覆盖的寄存器

当前版本必须完整覆盖以下有效地址：

- `1001-1011`
- `1021-1035`
- `1037`
- `1039-1042`
- `1048-1050`

### 7.2 禁止冲突项

以下冲突在实现和文档中均不得出现：

1. 将 `1001` 解释为“charging/idle”运行状态
2. 将 `1034` 与 `1050` 视作同一寄存器
3. 将 `1023`、`1026` 的组合字拆成未经厂家确认的布尔字段
4. 将 `1027` 按未确认单位直接上报为功率工程值

## 8. 文档协同关系

本文档定义：

- 软件边界
- 缓存结构
- 模块职责
- 命令流 / 数据流

其中具体字段定义以 `Modbus 设备模型规范 V1` 为准，云端报文结构以 `MQTT 网关通信协议 V1.0` 为准。

当点表变更时，必须按以下顺序更新：

1. `RS485与V2G充电桩通信点表.md`
2. `Modbus 设备模型规范 V1.md`
3. `MQTT 网关通信协议 V1.0.md`
4. `STM32 软件架构设计 V1.md`
