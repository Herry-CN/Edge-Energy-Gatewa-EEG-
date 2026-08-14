#ifndef __BSP_ESP8266_TEST_H
#define __BSP_ESP8266_TEST_H


#include "stm32f1xx.h"


/********************************** 用户需要设置的参数 **********************************/

/* ================= WiFi 层 ================= */
#define      macUser_ESP8266_ApSsid                       "feifei"
#define      macUser_ESP8266_ApPwd                        "lijialing"

/* ================= MQTT 标识 ================= */
/*
 * TODO: 出厂时替换成这台桩的真实唯一编号
 * 必须等于后端 charging_station.code / serial_number，大小写完全一致！
 * 建议：出厂烧到配置分区 / eFuse，临时开发先在这里硬编码改
 */
#define      MQTT_DEVICE_ID                               "PILE-001"

/* ================= MQTT Broker 网络层 ================= */
#define      MQTT_BROKERADDRESS                           "192.168.8.97"
#define      MQTT_PORT                                    1883
#define      MQTT_KEEPALIVE                               60
#define      MQTT_CLEAN_SESSION                           1
#define      MQTT_DEFAULT_QOS                             1
/* ESP8266 MQTT-AT 2.2.0 + current broker: QoS1 publish is stable, but QoS1 SUB
 * reproducibly triggers +MQTTDISCONNECTED right after the first subscribe packet.
 * Keep publish/reply on QoS1, and temporarily use QoS0 for subscribe as a
 * compatibility workaround until firmware/broker interoperability is resolved. */
#define      MQTT_SUBSCRIBE_QOS                           0
#define      MQTT_STATUS_RETAIN                           1

/* ================= MQTT 鉴权（私有协议纯明文，内网先跑通） ================= */
/* MQTT client-id must be globally unique on the broker.
 * Keep topic device id unchanged, and append a client-id suffix so this board
 * does not collide with other tools or devices using sim-pile-PILE-001. */
#define      MQTT_CLIENT_ID_PREFIX                        "sim-pile-"
#define      MQTT_CLIENT_ID_SUFFIX                        "-at"
#define      MQTT_CLIENT_ID                               MQTT_CLIENT_ID_PREFIX MQTT_DEVICE_ID MQTT_CLIENT_ID_SUFFIX  /* sim-pile-PILE-001-at */
#define      MQTT_USER_NAME                               "charge"
#define      MQTT_PASSWD                                  "123456"

/* ========== 协议开关：迁移期 EEG V1.0 与旧私有协议并行 ==========
 * 两个都开 = 双发：同一份数据同时走 eeg/... 新主题树和 /device/... 旧主题，
 * 后端切完、MQTTX 对照验证过之后，把 LEGACY 关掉即可，代码不用动。 */
#define      EEG_PROTO_ENABLE                             1
#define      LEGACY_PROTO_ENABLE                          1

/* ================= EEG V1.0 主题树（Doc/MQTT 网关通信协议 V1.0.md §3） =================
 * eeg/{site}/{gateway}/{device_type}/{device_id}/{channel}
 * device_type 取自文档固定枚举：gateway/charger/battery/inverter/meter/sensor
 * TODO: 出厂时把 site/gateway/device 换成真实编号（同 MQTT_DEVICE_ID 一样应落到配置区） */
#define      EEG_SITE_ID                                  "site01"
#define      EEG_GATEWAY_ID                               "gw001"
#define      EEG_DEVICE_TYPE                              "charger"
#define      EEG_DEVICE_ID                                "ch001"

#define      EEG_FW_VERSION                               "EEG-1.0.0"
#define      EEG_HW_VERSION                               "STM32F103+ESP8266"

#define      EEG_TOPIC_ROOT                               "eeg/" EEG_SITE_ID "/" EEG_GATEWAY_ID
#define      EEG_TOPIC_GW_STATUS                          EEG_TOPIC_ROOT "/gateway/" EEG_GATEWAY_ID "/status"   /* §5 */
#define      EEG_TOPIC_DEV                                EEG_TOPIC_ROOT "/" EEG_DEVICE_TYPE "/" EEG_DEVICE_ID
#define      EEG_TOPIC_DEV_STATUS                         EEG_TOPIC_DEV "/status"                               /* §6 */
#define      EEG_TOPIC_DEV_CMD                            EEG_TOPIC_DEV "/cmd"                                  /* §7 */
#define      EEG_TOPIC_DEV_ACK                            EEG_TOPIC_DEV "/ack"                                  /* §8 */
#define      EEG_TOPIC_DEV_EVENT                          EEG_TOPIC_DEV "/event"                                /* §9 */

/* §4 Retain：网关状态 / 设备状态 = true，命令 / ACK / 事件 = false */
#define      EEG_RETAIN_STATUS                            1
#define      EEG_RETAIN_TRANSIENT                         0

/* 网关状态是心跳性质的，不必跟设备状态一样 5s 一发 */
#define      EEG_GW_STATUS_INTERVAL_MS                    60000

/* ================= SNTP 对时（ts 字段要的是 Unix 秒） =================
 * 时区固定 0：AT+CIPSNTPTIME? 回的是可读时间串，按 UTC 解析才能直接换算 epoch。
 * 对不上时（没有外网）自动退化成开机秒数，见 EEG_TimeIsSynced()。 */
#define      EEG_SNTP_SERVER1                             "ntp.aliyun.com"
#define      EEG_SNTP_SERVER2                             "cn.pool.ntp.org"
#define      EEG_SNTP_RESYNC_MS                           21600000UL   /* 6h 重新校一次 */
#define      EEG_SNTP_MIN_VALID_YEAR                      2023         /* 小于此年份视为没对上 */

/* ================= §9 事件：过温告警（带回差，避免抖动刷屏） ================= */
#define      EEG_TEMP_ALARM_C                             60
#define      EEG_TEMP_CLEAR_C                             55
#define      EEG_EVT_CODE_OVER_TEMP                       201

/* ================= MQTT 主题（文档 §4.2 / §5.1 / §7.1 严格对应） ================= */
#define      MQTT_SUBSCRIBE_TOPIC_CTRL                    "/device/" MQTT_DEVICE_ID "/control"
#define      MQTT_SUBSCRIBE_TOPIC_CTRLL                   "/device/" MQTT_DEVICE_ID "/controll"
/* 兼容旧代码里的 MQTT_SUBSCRIBE_TOPIC 宏（实际订阅两条，主循环里显式再订阅 CTRLL）*/
#define      MQTT_SUBSCRIBE_TOPIC                         MQTT_SUBSCRIBE_TOPIC_CTRL

#define      MQTT_PUBLISH_TOPIC_STATUS                    "/device/" MQTT_DEVICE_ID "/status"
/* 兼容旧代码里的 MQTT_PUBLISH_TOPIC 宏名 */
#define      MQTT_PUBLISH_TOPIC                           MQTT_PUBLISH_TOPIC_STATUS

#define      MQTT_PUBLISH_TOPIC_REPLY                     "/device/" MQTT_DEVICE_ID "/control"

/* ================= 上报倍率（§5.1 整数倍率，不要浮点！） ================= */
/*   power  (kW) × 10   → 整数，单位 0.1kW      e.g. 6.5kW  → 65   */
/*   voltage(V ) × 10   → 整数，单位 0.1V       e.g. 220.0V → 2200 */
/*   current(A ) × 100  → 整数，单位 0.01A      e.g. 29.50A → 2950 */
#define      POWER_MULTIPLIER                             10
#define      VOLTAGE_MULTIPLIER                           10
#define      CURRENT_MULTIPLIER                           100

/* ================= onoff 状态枚举（文档 §5.1 / §6） ================= */
/*   0 = charging 充电中（继电器闭合）
 *   1 = idle     空闲待机（继电器断开、在线）
 *   2 = offline  离线/停止（收到停充命令 onoff=2 时上报）
 *  后端下发命令 value=0（开始充） / value=2（停止充）*/
#define      ONOFF_IDLE           1
#define      ONOFF_CHARGING       0
#define      ONOFF_OFFLINE        2


/********************************** 外部全局变量 ***************************************/
extern volatile uint8_t ucTcpClosedFlag;


/********************************** 测试函数声明 ***************************************/
void ESP8266_StaTcpClient_Unvarnish_ConfigTest(void);
void ESP8266_SendDHT11DataTest(void);

#endif /* __BSP_ESP8266_TEST_H */
