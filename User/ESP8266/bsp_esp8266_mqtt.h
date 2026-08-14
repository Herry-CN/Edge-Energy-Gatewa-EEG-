#ifndef __BSP_ESP8266_MQTT_H
#define __BSP_ESP8266_MQTT_H

#include "./common/common.h"
#include <stdio.h>  
#include <string.h>  
#include <stdbool.h>
#include "./dwt_delay/core_delay.h"
#include "./led/bsp_led.h" 
#include "./ESP8266/bsp_esp8266.h"

extern int led_value;//led状态值（兼容旧代码，正式逻辑改用 g_onoff_state）

/* 充电桩正式状态（文档 §5.1）：
 *   g_current_power / g_current_voltage / g_current_current 均为整数倍率上报值
 *   g_onoff_state：见 bsp_esp8266_test.h 里 ONOFF_* 枚举 */
extern uint32_t g_current_power;     /* power_kw × 10   (0.1kW 单位) */
extern uint32_t g_current_voltage;   /* voltage_v × 10  (0.1V  单位) */
extern uint32_t g_current_current;   /* current_a × 100 (0.01A 单位) */
extern uint8_t  g_onoff_state;       /* ONOFF_CHARGING / ONOFF_IDLE / ONOFF_OFFLINE */

extern uint8_t mqtt_flag;//mqtt连接标志

/* ── 下行帧队列 ─────────────────────────────────────────────────
 * 生产者只有 USART3 ISR，消费者只有主循环，所以 head/tail 各自单写，
 * 不需要临界区。队列独立于 AT 应答缓冲区：帧一旦入队，后续任何
 * ESP8266_ATFrame_Reset() 都冲不掉它。
 *
 * 之前用 pending/len 两个标志表示"有一帧待处理"，两者无法原子地一起
 * 清除：Dispatch 前清 pending、Dispatch 后清 len，而 Dispatch 里发 ACK
 * 要上百毫秒，期间到达的新命令会被 ISR 抓进来（置 len）再被主循环抹掉，
 * 留下 pending=1/len=0 —— 消费端要求 len>0 进不去，ISR 见 pending=1 不再抓，
 * 下行就此永久锁死。队列没有这种半状态。*/
#define MQTT_RX_SLOTS      4
#define MQTT_RX_SLOT_LEN   512

void     MQTT_RxQueue_Harvest ( void );                    /* ISR / 缓冲区复位前调用：切帧入队 */
bool     MQTT_RxQueue_Pop     ( char* out, int outsz );    /* 主循环调用：取一帧私有副本 */
void     MQTT_RxQueue_Reset   ( void );                    /* 清空队列与扫描水位 */
void     MQTT_RxScan_Reset    ( void );                    /* AT 缓冲区被清空时同步复位水位 */
extern volatile uint32_t g_mqtt_rx_dropped;                /* 队列满 / 帧过长而丢弃的下行帧数 */

/* ── 通用发布：整条 AT 命令 <256 字节走 AT+MQTTPUB，超了自动切 AT+MQTTPUBRAW ── */
bool ESP8266_MQTT_Publish  ( const char* topic, const char* payload, int qos, int retain );

/* ── 轻量 JSON 取值（不是完整解析器，要求 "key":value 紧凑出现） ── */
bool MQTT_JsonGetInt  ( const char* buf, const char* field, int* out );
bool MQTT_JsonGetStr  ( const char* buf, const char* field, char* out, int outsz );  /* 只取字符串值 */
bool MQTT_JsonGetText ( const char* buf, const char* field, char* out, int outsz );  /* 数字/字符串都取原文 */

/* MQTT AT 指令层接口 */
bool ESP8266_MQTT_USERCFG  ( void );         /* AT+MQTTUSERCFG (8 params). fallback: CLIENTID/USERNAME/PASSWORD 3 seperate cmds */
bool ESP8266_MQTT_CONNCFG  ( void );         /* AT+MQTTCONNCFG (8 params): keepalive=60, clean=true (doc §2 mandatory explicit) */
bool ESP8266_MQTT_CLEAN    ( void );         /* AT+MQTTCLEAN : close MQTT session on module side before reconnect */
bool ESP8266_MQTT_CONN     ( void );         /* AT+MQTTCONN : connect broker:port, reconnect=0 (manual) */
bool ESP8266_MQTT_SUB      ( void );         /* AT+MQTTSUB : dual-topic control + controll (QoS=1) per §4.2 */
bool ESP8266_MQTT_PUB_STATUS ( void );       /* AT+MQTTPUB : status JSON 4-field integer × multiplier, QoS1 retain1 */
bool ESP8266_MQTT_PUB_REPLY(int value, const char* result); /* control reply JSON -> /control, QoS1 retain0 */
bool ESP8266_MQTT_RECV     ( const char* frame ); /* legacy downlink: ONLY accept type=command + name=onoff + value∈{0,2} */
bool ESP8266_MQTT_Dispatch ( const char* frame ); /* route one +MQTTSUBRECV frame by topic (EEG cmd vs legacy control) */


#endif /* __BSP_ESP8266_MQTT_H */
