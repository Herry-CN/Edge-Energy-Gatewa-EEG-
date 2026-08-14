#ifndef __BSP_EEG_PROTO_H
#define __BSP_EEG_PROTO_H

/**
  ******************************************************************************
  * @file    bsp_eeg_proto.h
  * @brief   EEG V1.1 报文层（Doc/MQTT 网关通信协议 V1.1.md §5~§9）
  *
  * 只负责「主题 + JSON 报文 + 命令语义」，具体怎么把字节送出去由
  * ESP8266_MQTT_Publish() 决定（短报文 AT+MQTTPUB，长报文 AT+MQTTPUBRAW）。
  *
  * 主题树、站点/网关/设备编号、SNTP 服务器、告警阈值都在 bsp_esp8266_test.h。
  ******************************************************************************
  */

#include "stm32f1xx.h"
#include <stdbool.h>

/* ── 设备对象模型缓存（Doc/Modbus 设备模型规范 V1.1.md 充电桩子集）──
 * 统一保存“原始寄存器语义 + 上报所需倍率整数”，避免 MQTT 层重新猜测含义。 */
extern uint16_t g_state_code;            /* 1002 原始状态码 */
extern uint16_t g_fault_code;            /* 1003 原始故障码 */
extern uint16_t g_enable;                /* 1024 */
extern uint16_t g_charge_power_setpoint; /* 1025，0.1kW */
extern uint32_t g_energy_charge;         /* 1036，0.1kWh  -> 上报一位小数 */
extern uint32_t g_energy_discharge;      /* 1038，0.01kWh -> 上报两位小数 */
extern uint8_t  g_soc;                   /* 1032，% */
extern int16_t  g_temperature;           /* 1040 还原后的实际温度 */
extern uint16_t g_mode_code;             /* 1050 原始模式码 */
extern uint16_t g_start_stop;            /* 1035 启停状态 */
extern uint16_t g_start_stop_control;    /* 1051 启停控制 */

/* ── 时间戳 ──
 * ts 字段要的是 Unix 秒。SNTP 对上就是真实 epoch，对不上（无外网）自动退化成
 * 开机秒数，用 EEG_TimeIsSynced() 区分，日志里会明确提示。 */
void        EEG_TimeInit             ( void );   /* AT+CIPSNTPCFG，联网后调用一次 */
bool        EEG_TimeSync             ( void );   /* AT+CIPSNTPTIME? 拉一次时间并锁存 */
void        EEG_TimeTask             ( void );   /* 主循环调用：未同步时重试、到期重新校时 */
uint32_t    EEG_Timestamp            ( void );
bool        EEG_TimeIsSynced         ( void );

/* ── 报文（§5/§6/§8/§9）── */
bool        EEG_PublishGatewayStatus ( void );
bool        EEG_PublishDeviceStatus  ( void );
bool        EEG_PublishAck           ( const char* id_raw, bool ok, int code, const char* msg );
bool        EEG_PublishEvent         ( const char* level, int code, const char* msg, int temperature );

/* ── §7 控制命令：解析 + 执行 + 回 ACK ── */
bool        EEG_HandleCommand        ( const char* json );

/* ── 派生量：电量积分 + SOC 推演，每个上报周期调一次 ── */
void        EEG_UpdateDerived        ( void );
/* ── §9 过温告警（带回差） ── */
void        EEG_CheckTempAlarm       ( void );

#endif /* __BSP_EEG_PROTO_H */
