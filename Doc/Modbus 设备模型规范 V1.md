# Modbus 设备模型规范 V1

## 1. 文档定位

本文档定义 `RS485 与 V2G 充电桩通信点表` 在 STM32 网关中的标准化设备模型、寄存器映射、缩放规则和读写边界，作为以下文档的统一数据基准：

- `Doc/RS485与V2G充电桩通信点表.md`
- `Doc/MQTT 网关通信协议 V1.0.md`
- `Doc/STM32 软件架构设计 V1.md`

本文档只描述充电桩 `Charger` 设备模型；后续接入其它 Modbus 设备时，应新增独立模型规范，不得复用或挤占本模型字段。

## 2. 地址基准（写死，禁止再改口径）

本项目接入的充电桩采用 **4xxxx 保持寄存器** 编号。点表里的 `1001` 等是**逻辑地址 N**，不是报文里再另加一套编号。

| 名称 | 公式 | 示例（点表 1001） |
| --- | --- | --- |
| 逻辑地址 N | 点表「寄存器地址」列 | `1001` |
| 4x 真实地址 | **N + 40001** | `41002` |
| RTU 报文 PDU 起始地址 | **4x 真实地址 − 40001 = N** | 功能码 0x03/0x06 发 `0x03E9` |

约束：

1. 点表、模型字段、MQTT `registers` 的键，**一律写 N**（1001…1050），禁止写 41002。
2. STM32 主站组帧默认 **PDU = N**（即 `40001+N` 再减回 40001）。宏写死：

```c
#define MB_HOLDING_BASE     40001u   /* 4x 区基址，本桩真实地址 = N + 40001 */
#define MB_PDU_FROM_LOGIC(n) ((uint16_t)(n))           /* 报文地址 = N */
#define MB_PLC_FROM_LOGIC(n) ((uint16_t)(40001u + (n))) /* 仅用于人机/说明书 */
```

3. 若联调发现从站要求 **报文内直接出现 41002**（把 4x 号塞进 PDU），只允许改一处：

```c
#define MB_PDU_FROM_LOGIC(n) ((uint16_t)(40001u + (n)))
```

不得把 40001 散落到业务层或 MQTT。差 1 的 0-based/1-based 问题用 `CHG_REG_OFFSET`（0 或 −1）单独处理，与 40001 基址无关。

4. PC 端 Modbus Server / 厂家说明书若显示 `41002`，与点表 `1001`、报文 `0x03E9` 是同一点，不是三个点。

## 3. 建模原则

### 3.1 原始寄存器优先

STM32 侧必须完整缓存点表中已定义的全部有效寄存器原始值。对于以下类型字段，不允许在协议层擅自猜测拆分规则：

- 组合字
- 位图字
- 厂家私有编码字

因此以下字段统一按 `raw word` 建模，保持 `uint16_t` 原值：

- `capability_word`（1003）
- `enable_word`（1023）
- `gun_attribute_state_word`（1026）
- `gun_power_limit_raw`（1027）
- `start_stop_control_raw`（1050 写入值）

### 3.2 标准化值与原始值并存

对于单位和倍率明确的寄存器，应用层在保留原始值的同时，需生成标准化工程值供 MQTT 上报和业务逻辑使用。

示例：

- `1028` 原始值 `3801` -> 标准化值 `380.1 V`
- `1029` 原始值 `3125` -> 标准化值 `31.25 A`
- `1039` 原始值 `75` -> 标准化值 `35 C`，计算规则为 `raw - 40`

### 3.3 状态语义分离

为避免状态混淆，三个易冲突字段必须严格区分：

- `1001 charger_state`：桩总体健康状态，枚举 `normal/fault/alarm`
- `1034 start_stop_state`：当前启停状态，枚举 `start/standby/stop`
- `1050 start_stop_control_raw`：启停控制写寄存器，写入值语义由设备厂家协议补充定义

`1050` 是控制寄存器，不等同于 `1034` 的状态寄存器，禁止直接将两者视为同一含义。

## 4. 设备对象模型

### 4.1 Charger

```Plain Text
Charger
 ├── charger_state
 ├── fault_code
 ├── capability_word
 ├── module_count
 ├── rated_charge_power
 ├── rated_discharge_power
 ├── bms_charge_power_demand
 ├── pile_output_power
 ├── input_voltage
 ├── input_current
 ├── input_power
 ├── offline_max_discharge_power
 ├── offline_max_charge_power
 ├── enable_word
 ├── charge_power_setpoint
 ├── discharge_power_setpoint
 ├── gun_attribute_state_word
 ├── gun_power_limit_raw
 ├── output_voltage
 ├── output_current
 ├── output_power
 ├── vehicle_soc
 ├── bms_voltage_demand
 ├── bms_current_demand
 ├── start_stop_state
 ├── energy_charge
 ├── energy_discharge
 ├── gun_temperature
 ├── eta_charge_complete_min
 ├── charge_duration_min
 ├── session_charge_energy
 ├── target_soc
 ├── work_mode
 └── start_stop_control_raw
```

### 4.2 状态枚举定义

#### 1001 `charger_state`

| 原始值 | 文本值 |
| ------ | ------ |
| 0 | `normal` |
| 1 | `fault` |
| 2 | `alarm` |

#### 1034 `start_stop_state`

| 原始值 | 文本值 |
| ------ | ------ |
| 0 | `start` |
| 1 | `standby` |
| 2 | `stop` |

#### 1049 `work_mode`

| 原始值 | 文本值 |
| ------ | ------ |
| 0 | `chg` |
| 1 | `v2g` |

## 5. 寄存器映射总表

| 寄存器 | 模型字段 | R/W | 原始单位 | 标准化字段 | 标准化单位 | 规则/说明 |
| ------ | -------- | --- | -------- | ---------- | ---------- | --------- |
| 1001 | `charger_state` | R | / | `charger_state` | enum | 0=`normal`，1=`fault`，2=`alarm` |
| 1002 | `fault_code` | R | / | `fault_code` | uint16 | 厂家故障码原值透传 |
| 1003 | `capability_word` | R | / | `capability_word` | uint16 | “是否支持放电功能、枪数量”组合编码，保持原值 |
| 1004 | `module_count` | R | / | `module_count` | uint16 | 充电模块数量 |
| 1005 | `rated_charge_power_raw` | R | 0.1kW | `rated_charge_power_kw` | kW | `raw / 10.0` |
| 1006 | `rated_discharge_power_raw` | R | 0.1kW | `rated_discharge_power_kw` | kW | `raw / 10.0` |
| 1007 | `bms_charge_power_demand_raw` | R | 0.1kW | `bms_charge_power_demand_kw` | kW | `raw / 10.0` |
| 1008 | `pile_output_power_raw` | R | 0.1kW | `pile_output_power_kw` | kW | `raw / 10.0` |
| 1009 | `input_voltage_raw` | R | 0.1V | `input_voltage_v` | V | `raw / 10.0` |
| 1010 | `input_current_raw` | R | 0.01A | `input_current_a` | A | `raw / 100.0` |
| 1011 | `input_power_raw` | R | 0.1kW | `input_power_kw` | kW | `raw / 10.0` |
| 1021 | `offline_max_discharge_power_raw` | R/W | 0.1kW | `offline_max_discharge_power_kw` | kW | `raw / 10.0` |
| 1022 | `offline_max_charge_power_raw` | R/W | 0.1kW | `offline_max_charge_power_kw` | kW | `raw / 10.0` |
| 1023 | `enable_word` | R/W | / | `enable_word` | uint16 | “是否允许充电/放电”组合编码，保持原值 |
| 1024 | `charge_power_setpoint_raw` | R/W | 0.1kW | `charge_power_setpoint_kw` | kW | `raw / 10.0` |
| 1025 | `discharge_power_setpoint_raw` | R/W | 0.1kW | `discharge_power_setpoint_kw` | kW | `raw / 10.0` |
| 1026 | `gun_attribute_state_word` | R | / | `gun_attribute_state_word` | uint16 | “枪属性、当前状态”组合编码，保持原值 |
| 1027 | `gun_power_limit_raw` | R | / | `gun_power_limit_raw` | uint16 | 点表未给出工程单位，保持原值 |
| 1028 | `output_voltage_raw` | R | 0.1V | `output_voltage_v` | V | `raw / 10.0` |
| 1029 | `output_current_raw` | R | 0.01A | `output_current_a` | A | `raw / 100.0` |
| 1030 | `output_power_raw` | R | 0.1kW | `output_power_kw` | kW | `raw / 10.0` |
| 1031 | `vehicle_soc_raw` | R | % | `vehicle_soc_pct` | % | 直接使用原值 |
| 1032 | `bms_voltage_demand_raw` | R | 0.1V | `bms_voltage_demand_v` | V | `raw / 10.0` |
| 1033 | `bms_current_demand_raw` | R | 0.01A | `bms_current_demand_a` | A | `raw / 100.0` |
| 1034 | `start_stop_state` | R | / | `start_stop_state` | enum | 0=`start`，1=`standby`，2=`stop` |
| 1035 | `energy_charge_raw` | R | 0.1kWh | `energy_charge_kwh` | kWh | `raw / 10.0` |
| 1037 | `energy_discharge_raw` | R | 0.01kWh | `energy_discharge_kwh` | kWh | `raw / 100.0` |
| 1039 | `gun_temperature_raw` | R | C（含 -40 偏移） | `gun_temperature_c` | C | `raw - 40` |
| 1040 | `eta_charge_complete_min` | R | min | `eta_charge_complete_min` | min | 直接使用原值 |
| 1041 | `charge_duration_min` | R | min | `charge_duration_min` | min | 直接使用原值 |
| 1042 | `session_charge_energy_raw` | R | 0.01kWh | `session_charge_energy_kwh` | kWh | `raw / 100.0` |
| 1048 | `target_soc_raw` | R/W | 0.1% | `target_soc_pct` | % | `raw / 10.0` |
| 1049 | `work_mode` | R/W | / | `work_mode` | enum | 0=`chg`，1=`v2g` |
| 1050 | `start_stop_control_raw` | R/W | / | `start_stop_control_raw` | uint16 | 控制值定义以厂家协议为准，不得套用 1034 枚举 |

说明：

- 点表中的 `1036`、`1038` 为空地址，当前版本不纳入模型。
- 所有 `R/W` 字段既可作为状态回读字段，也可作为控制结果校验字段。

## 6. 读写寄存器组规划

为兼顾 Modbus RTU 帧长、轮询效率和错误隔离，按以下分组读取。周期与时隙见《STM32 软件架构设计 V1》第 5.1 节，三份文档必须相同。

| 分组 | 逻辑地址范围 | 4x 真实地址 | 数量 | 周期 | 用途 |
| ---- | -------- | -------- | --- | --- | ---- |
| G1 | 1001-1011 | 41002-41012 | 11 | **200 ms** | 桩健康、额定、进线电气 |
| G2 | 1021-1035 | 41022-41036 | 15 | **500 ms** | 设定回读、枪输出、SOC、启停状态、充电电表 |
| G3 | 1037-1042 | 41038-41043 | 6 | **1000 ms** | 放电电表、温度、时长、本次电量 |
| G4 | 1048-1050 | 41049-41051 | 3 | **1000 ms** | 目标 SOC、模式、启停控制回读 |

9600 8-N-1 下一帧往返约 40–80 ms。G1 若坚持 **100 ms** 会占满总线，G2/G3/G4 无法调度，故 G1 写死为 **200 ms**。MQTT `status` **2 s**、`registers` **10 s**，都只读缓存，不另起 Modbus 事务。

写操作白名单如下：

- 1021
- 1022
- 1023
- 1024
- 1025
- 1048
- 1049
- 1050

除上述地址外，禁止通过网关对充电桩执行写寄存器操作。

## 7. 缓存与校验要求

### 7.1 缓存要求

STM32 应维护两类数据：

1. 原始缓存 `raw registers`
2. 标准化视图 `engineering view`

原始缓存用于：

- Modbus 回读一致性检查
- MQTT `registers` 原值镜像
- 追溯厂家编码字段

标准化视图用于：

- 云端状态上报
- 本地逻辑判断
- 命令参数合法性校验

### 7.2 合法性校验

对点表未提供范围或位定义的寄存器，网关不得自行发明限制条件，只做以下基础校验：

- 地址必须在白名单内
- 写入值必须在 `uint16_t` 范围内
- 倍率换算后反算得到的原始值必须无歧义

### 7.3 一致性要求

`MQTT 网关通信协议 V1.0` 和 `STM32 软件架构设计 V1` 中出现的以下字段名必须与本文一致：

- `charger_state`
- `start_stop_state`
- `work_mode`
- `enable_word`
- `target_soc_pct`
- `charge_power_setpoint_kw`
- `discharge_power_setpoint_kw`
- `start_stop_control_raw`

后续若新增字段，必须先修改本文，再修改其它文档。
