#ifndef __BSP_EEG_PROTO_H
#define __BSP_EEG_PROTO_H

/**
  ******************************************************************************
  * @file    bsp_eeg_proto.h
  * @brief   EEG V1.0 报文层（Doc/MQTT 网关通信协议 V1.0.md §5~§9）
  *
  * 只负责「主题 + JSON 报文 + 命令语义」，具体怎么把字节送出去由
  * ESP8266_MQTT_Publish() 决定（短报文 AT+MQTTPUB，长报文 AT+MQTTPUBRAW）。
  *
  * 主题树、站点/网关/设备编号、SNTP 服务器、告警阈值都在 bsp_esp8266_test.h。
  ******************************************************************************
  */

#include "stm32f1xx.h"
#include <stdbool.h>

/* ── 设备对象模型缓存（由 charger_export_cache 从寄存器刷新）──
 * 电压/电流/功率仍用整数倍率：0.1V / 0.01A / 0.1kW。 */
extern uint32_t g_energy_charge;         /* 1035，kWh × 10  */
extern uint32_t g_energy_discharge;      /* 1037，kWh × 100 */
extern uint8_t  g_soc;
extern int16_t  g_temperature;           /* 1039 raw - 40 */
extern uint16_t g_fault_code;
extern uint16_t g_charger_state;         /* 1001 桩状态 0正常 1故障 2报警 */
extern uint16_t g_enable_word;           /* 1023 */
extern uint16_t g_start_stop_state;      /* 1034 */
extern uint16_t g_start_stop_control;    /* 1050 R/W 启停 1启动 0停止 */
extern uint16_t g_work_mode;             /* 1049 */
extern uint16_t g_capability_word;       /* 1003 */
extern uint16_t g_module_count;          /* 1004 */
extern uint32_t g_input_voltage;         /* 1009，0.1V  进线，不是 voltage */
extern uint32_t g_input_current;         /* 1010，0.01A 进线，不是 current */
extern uint32_t g_input_power;           /* 1011，0.1kW */
extern uint32_t g_charge_power_x10;      /* 1024，0.1kW */

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
bool        EEG_PublishRegisters     ( void );
bool        EEG_PublishAck           ( const char* id_raw, bool ok, int code, const char* msg );
bool        EEG_PublishEvent         ( const char* level, int code, const char* msg, int temperature );

/* ── §7 控制命令：解析 + 执行 + 回 ACK ── */
bool        EEG_HandleCommand        ( const char* json );

/* ── 派生量：电量积分 + SOC 推演，每个上报周期调一次 ── */
void        EEG_UpdateDerived        ( void );
/* ── §9 过温告警（带回差） ── */
void        EEG_CheckTempAlarm       ( void );

#endif /* __BSP_EEG_PROTO_H */
