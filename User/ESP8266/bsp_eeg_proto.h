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

/* ── 设备对象模型缓存（Doc/Modbus 设备模型规范 V1.md 充电桩子集）──
 * 现在由 DHT11 + 模拟值填充；接上 Modbus 之后改由寄存器缓存刷新，
 * 报文层一行都不用动。电压/电流/功率仍沿用 g_current_* 那套整数倍率。 */
extern uint32_t g_energy_charge;    /* 本次充电电量 kWh × 100 */
extern uint8_t  g_soc;              /* 电池 SOC，% */
extern int16_t  g_temperature;      /* 温度 ℃（当前取板载 DHT11） */
extern uint16_t g_fault_code;       /* 0 = 无故障 */

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
