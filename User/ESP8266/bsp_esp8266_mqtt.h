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
extern volatile uint8_t g_mqtt_rx_pending; /* USART3 IDLE ISR marks pending MQTT downlink frame */

extern uint8_t mqtt_flag;//mqtt连接标志

/* MQTT AT 指令层接口 */
bool ESP8266_MQTT_USERCFG  ( void );         /* AT+MQTTUSERCFG (8 params). fallback: CLIENTID/USERNAME/PASSWORD 3 seperate cmds */
bool ESP8266_MQTT_CONNCFG  ( void );         /* AT+MQTTCONNCFG (8 params): keepalive=60, clean=true (doc §2 mandatory explicit) */
bool ESP8266_MQTT_CLEAN    ( void );         /* AT+MQTTCLEAN : close MQTT session on module side before reconnect */
bool ESP8266_MQTT_CONN     ( void );         /* AT+MQTTCONN : connect broker:port, reconnect=0 (manual) */
bool ESP8266_MQTT_SUB      ( void );         /* AT+MQTTSUB : dual-topic control + controll (QoS=1) per §4.2 */
bool ESP8266_MQTT_PUB_STATUS ( void );       /* AT+MQTTPUB : status JSON 4-field integer × multiplier, QoS1 retain1 */
bool ESP8266_MQTT_PUB_REPLY(int value, const char* result); /* control reply JSON -> /control, QoS1 retain0 */
bool ESP8266_MQTT_RECV     ( void );         /* parse downlink, ONLY accept type=command + name=onoff + value∈{0,2} */


#endif /* __BSP_ESP8266_MQTT_H */
