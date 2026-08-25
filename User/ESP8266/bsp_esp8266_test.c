/**
  ******************************************************************************
  * @file    bsp_esp8266_test.c
  * @brief   充电桩业务流程（完全替换阿里云旧私有协议，文档 §2~§11）
  *
  * 对外主入口：
  *   ESP8266_StaTcpClient_Unvarnish_ConfigTest() — 启动阶段调用一次（阻塞，直到全链路 OK）
  *   ESP8266_SendDHT11DataTest()                  — 主循环每 5s 调用（publish status）
  *
  * 掉线恢复三层防护：
  *   ① publish 失败计数 → 3 次失败 = 链路可能挂
  *   ② 30s 健康检测     → AT+MQTTCONN? 未在线 = 尝试重连（不复位模块，省时间）
  *   ③ 看门狗最终重建   → ①+② 都失败 = CH_PD 拉低 500ms 模块硬复位 + 完整重建链路
  ******************************************************************************
  */
#include "./ESP8266/bsp_esp8266_test.h"
#include "./ESP8266/bsp_esp8266.h"
#include <stdio.h>
#include <string.h>
#include <stdbool.h>
#include "./dht11/bsp_dht11.h"
#include "./led/bsp_led.h"
#include "./usart/bsp_debug_usart.h"
#include "./ESP8266/bsp_esp8266_mqtt.h"
#include "./ESP8266/bsp_eeg_proto.h"
#include "./devices/charger.h"
#include "./config/bsp_config.h"
#include "./wdg/bsp_iwdg.h"

/* ================ 对外/导入全局变量 ================ */
DHT11_Data_TypeDef DHT11_Data;
volatile uint8_t ucTcpClosedFlag = 0;

extern uint8_t  publish_flag;          /* stm32f1xx_it.c SysTick 每 5000ms 置 1 */
extern uint32_t g_current_power;      /* kW × 10  */
extern uint32_t g_current_voltage;    /* V  × 10  */
extern uint32_t g_current_current;    /* A  × 100 */
extern uint8_t  g_onoff_state;        /* ONOFF_*   */

/* ================ 掉线恢复内部计数器 ================ */
static uint8_t  s_pub_fail_cnt = 0;   /* publish 连续失败次数（≥3 → 触发看门狗重建）*/
static uint8_t  s_health_fail = 0;    /* 健康检测连续失败 */
static uint32_t s_last_health_ms = 0;/* 上一次健康检测时间戳 */
static uint32_t s_last_pub_ok_ms = 0;/* 最近一次 publish 成功时间戳 */
static uint32_t s_last_gw_ms = 0;    /* 上一次网关状态心跳时间戳（§5）*/
static uint32_t s_last_rx_drop = 0;  /* 上次报告过的下行丢帧数 */
#define HEALTH_INTERVAL_MS       30000   /* 30s 一次健康检测（§8：3×上报间隔 ≈ 15s 保守取 30s）*/
#define PUBLISH_FAIL_THRESHOLD   3
#define HEALTH_FAIL_THRESHOLD    2

/* ================ 调试/诊断模式开关 ================
 * 0 = PRODUCTION MODE (full MQTT startup run, business loop publish/recv)
 * 1 = DIAGNOSTIC MODE (print AT syntax tests, block forever at LED1 blink) */
#define ESP8266_RUN_DIAGNOSTIC    0

static void mqtt_prepare_reconnect(void);  /* 断网后 CLEAN + WiFi + USERCFG */

/* ================ 内部：链路全重建（看门狗兜底）================ */
static void rebuild_full_chain(void)
{
    printf("\r\n\r\n>>> [RECONNECT] Watchdog triggered FULL CHAIN rebuild ......\r\n");
    /* Hard module power-down 600ms + re-init UART3 pins */
    macESP8266_CH_DISABLE();
    LED1_ON;               /* Red LED on = rebuilding */
    HAL_Delay(600);        /* Long enough to drain ESP8266 internal caps */
    LED1_OFF;

    ESP8266_Init();
    s_pub_fail_cnt = 0;
    s_health_fail = 0;
    ucTcpClosedFlag = 0;
    g_onoff_state   = ONOFF_IDLE;
    g_current_power = 0;
    g_current_current = 0;
    g_current_voltage = 0;
    LED2_OFF;

    /* Re-run full startup sequence (blocks until success) */
    ESP8266_StaTcpClient_Unvarnish_ConfigTest();
    printf(">>> [RECONNECT] FULL CHAIN rebuild DONE OK\r\n\r\n");
}

/* ================ 内部：30s 健康检测 ================
 * Query via AT+MQTTCONN? for current link state.
 * If not online: first try quick CONN+SUB reconnect; if still fails -> watchdog full rebuild. */
static void do_health_check(void)
{
    if (HAL_GetTick() - s_last_health_ms < HEALTH_INTERVAL_MS) return;
    s_last_health_ms = HAL_GetTick();

    /* 下行队列被打满或帧超长时唯一的外部信号，别让它悄悄丢 */
    if (g_mqtt_rx_dropped != s_last_rx_drop) {
        printf("[HEALTH] downlink frames dropped: %lu (queue full or frame >= %d bytes)\r\n",
               (unsigned long)g_mqtt_rx_dropped, (int)MQTT_RX_SLOT_LEN);
        s_last_rx_drop = g_mqtt_rx_dropped;
    }

    /* A recent successful publish is stronger evidence than MQTTCONN?. */
    if ((HAL_GetTick() - s_last_pub_ok_ms) < HEALTH_INTERVAL_MS) {
        s_health_fail = 0;
        return;
    }

    if (!ucTcpClosedFlag && ESP8266_MQTT_IsOnline()) {
        s_health_fail = 0;
        return;
    }
    s_health_fail++;
    printf("\r\n[HEALTH] MQTT NOT ONLINE. fail=%u/%u. Try quick reconnect...\r\n",
           s_health_fail, HEALTH_FAIL_THRESHOLD);

    mqtt_prepare_reconnect();
    if (ESP8266_MQTT_CONN() && ESP8266_MQTT_SUB()) {
        s_health_fail = 0;
        MQTT_RxQueue_Reset();
        mqtt_flag = 1;
        printf("[HEALTH] quick reconnect OK (wifi+USERCFG+CONNCFG+CONN+SUB)\r\n");
        return;
    }
    if (s_health_fail >= HEALTH_FAIL_THRESHOLD) {
        rebuild_full_chain();
    }
}

/* ================ 内部：诊断模式（确认 ESP8266 新固件 AT MQTT 语法版本） ================ */
#if ESP8266_RUN_DIAGNOSTIC
static void diag_send(const char* cmd)
{
    printf("\r\n--- TX: %s\r\n", cmd);
    ESP8266_Cmd((char*)cmd, "OK", "ERROR", 1200);
    printf("--- RX: [%s]\r\n", strEsp8266_Fram_Record.Data_RX_BUF);
}
static void diagnostic_mode(void)
{
    printf("\r\n============================================================\r\n"
             "  ESP8266 MQTT-AT FORMAT DIAGNOSTIC (no =? syntax support).\r\n"
             "  We SEND ACTUAL COMMANDS, compare OK/ERROR with user's\r\n"
             "  USB-TTL real test log file mqtt指令测试 lines 123/179.\r\n"
             "============================================================\r\n");

    /* 0) Reset ESP8266 first: 100ms RST low -> 500ms boot delay to clear boot garbage */
    printf("\r\n[PRE] ESP8266 hard reset (RST 100ms low pulse) to clear boot garbage.\r\n");
    macESP8266_RST_LOW_LEVEL(); HAL_Delay(100);
    macESP8266_RST_HIGH_LEVEL(); HAL_Delay(700);
    /* Flush any leftover boot bytes from USART3 ring */
    ESP8266_ATFrame_Reset();

    /* 1) AT basic */
    diag_send("AT");
    diag_send("AT+GMR");

    /* --- MQTT ACTUAL FORMAT TESTS (use EXACT syntax from user's USB-TTL test log):
     *   Line 123: AT+MQTTUSERCFG=0,1,"ESP8266","test","123456",0,0,"" -> OK
     *   Line 179: AT+MQTTCONNCFG=0,60,0,"","","",0,0                -> OK
     *   Line 173: AT+MQTTCONN=0,"broker",1883,0 -> ERROR (wrong broker in log, syntax OK)
     * We use actual syntax with dummy values to verify parser ACCEPTS parameter counts.
     *   MQTTCONN will error because network/WiFi down (not syntax). That's expected. */
    printf("\r\n--- [A] USERCFG 8-param syntax (line 123 OK in user's test)\r\n");
    diag_send("AT+MQTTUSERCFG=0,1,\"sim-pile-TEST\",\"charge\",\"123456\",0,0,\"\"");

    printf("\r\n--- [B] CONNCFG: TRY 3 SYNTAX VARIANTS to find which this firmware accepts.\r\n"
             "    B1 = user L179 8-param, B2 = official 7-param clean=0, B3 = 7-param clean=1 (inverted).\r\n"
             "    (We upgraded business-mode CONNCFG already has same 3 variants internally).\r\n");
    diag_send("AT+MQTTCONNCFG=0,60,0,\"\",\"\",\"\",0,0");   /* B1: user L179 8-param */
    diag_send("AT+MQTTCONNCFG=0,60,0,\"\",\"\",0,0");       /* B2: official 7-param clean=0 */
    diag_send("AT+MQTTCONNCFG=0,60,1,\"\",\"\",0,0");       /* B3: 7-param clean=1 (inverted logic */

    printf("\r\n--- [C] MQTTCLIENTID/USERNAME/PASSWORD seperate-cmd fallback syntax\r\n"
             "    (listed in AT+CMD? indexes #71..#73 in user's log).\r\n");
    diag_send("AT+MQTTCLIENTID=0,\"sim-pile-TEST2\"");
    diag_send("AT+MQTTUSERNAME=0,\"charge\"");
    diag_send("AT+MQTTPASSWORD=0,\"123456\"");

    printf("\r\n--- [D] AT+MQTTSUB short form syntax count test (topic=dummy, qos=1)\r\n"
             "    This will ERROR since WiFi not connected (not subscribed yet).\r\n"
             "    Looking for: syntax ERROR means param-count wrong.\r\n");
    diag_send("AT+MQTTSUB=0,\"/device/TEST/status\",1");

    printf("\r\n--- [E] AT+MQTTPUB short form syntax (payload contains comma escape)\r\n"
             "    Expects syntax OK format first.\r\n");
    diag_send("AT+MQTTPUB=0,\"/device/TEST/status\",\"{\\\"a\\\":1\\,\\\"b\\\":2}\",1,0");

    printf("\r\n============================================================\r\n"
             "  Diagnostic done. Program waits here (LED1 blink = OK).\r\n"
             "  [GUIDE]: If diag A and B return OK -> our primary syntax matches firmware perfectly.\r\n"
             "           If A errors but C (3 cmds) OK -> use fallback path.\r\n"
             "============================================================\r\n");
    while(1) {
        LED1_TOGGLE;
        HAL_Delay(500);
    }
}
#endif

/* WiFi/MQTT 失败时留窗口给 USART1 config CLI */
static void cfg_wait_window_ms(uint32_t ms)
{
    uint32_t t0 = HAL_GetTick();

    while ((HAL_GetTick() - t0) < ms) {
        IWDG_Feed();
        Config_CliService();
    }
}

/* 断网后：WiFi 仍有 IP（CIPSTATUS 2/3/4）绝不能再 CWJAP，否则会 WIFI DISCONNECT
 * 把 MQTT 一起掐掉。只有 STATUS:5 才等自恢复 / 再关联。已在线则不 CLEAN。 */
static void mqtt_prepare_reconnect(void)
{
    const EegNetConfig *c = Config_Get();

    if (ESP8266_MQTT_IsOnline()) {
        printf("[RECONN] MQTT already online, skip CLEAN/USERCFG\r\n");
        return;
    }

    ESP8266_MQTT_CLEAN();
    cfg_wait_window_ms(300);

    if (!ESP8266_WifiEnsure((char *)c->wifi_ssid, (char *)c->wifi_pwd)) {
        printf("[RECONN] WiFi ensure FAIL ssid=%s\r\n", c->wifi_ssid);
        return;
    }

    if (!ESP8266_MQTT_USERCFG()) {
        printf("[RECONN] USERCFG failed, will retry on next CONN round\r\n");
    }
    ESP8266_MQTT_CONNCFG();
    cfg_wait_window_ms(150);
}

void ESP8266_ApplyNetworkConfig(void)
{
    static uint8_t busy = 0;
    const EegNetConfig *c;

    if (busy) {
        printf("[CFG] apply already in progress\r\n");
        return;
    }
    busy = 1;
    mqtt_flag = 0;
    Config_SetLogVerbose(1);
    c = Config_Get();

    printf("[CFG] reconnect wifi=%s  mqtt=%s:%u\r\n",
           c->wifi_ssid, c->mqtt_host, (unsigned)c->mqtt_port);

    while (!ESP8266_WifiEnsure((char *)c->wifi_ssid, (char *)c->wifi_pwd)) {
        printf("[CFG] WiFi join FAIL. config set wifi.ssid / wifi.password\r\n");
        cfg_wait_window_ms(2000);
        c = Config_Get();
    }
    mqtt_prepare_reconnect();
    while (!ESP8266_MQTT_CONN()) {
        printf("[CFG] MQTT CONN FAIL. config set mqtt.host / mqtt.port / mqtt.user\r\n");
        cfg_wait_window_ms(1500);
        mqtt_prepare_reconnect();
    }
    while (!ESP8266_MQTT_SUB()) {
        printf("[CFG] MQTT SUB FAIL, prepare + CONN then retry SUB\r\n");
        mqtt_prepare_reconnect();
        (void)ESP8266_MQTT_CONN();
        cfg_wait_window_ms(300);
    }

    MQTT_RxQueue_Reset();
    mqtt_flag = 1;
    s_last_health_ms = HAL_GetTick();
    s_last_pub_ok_ms = HAL_GetTick();
    busy = 0;
    Config_SetLogVerbose(0);
    printf("[CFG] apply done, MQTT online\r\n"
           "[LOG] USART1 quiet  (config show / help;  log verbose  to restore dumps)\r\n");
}

/* ================ 对外：启动阶段一次性初始化（阻塞直到全链路 OK） ================ */
void ESP8266_StaTcpClient_Unvarnish_ConfigTest(void)
{
    const EegNetConfig *c = Config_Get();

    printf("\r\n\r\n================== CHARGING PILE MQTT STARTUP ==================\r\n"
             "\r\n[WiFi SSID] : %s"
             "\r\n[Device ID] : %s"
             "\r\n[Broker]    : %s:%u"
             "\r\n==============================================================\r\n",
             c->wifi_ssid, MQTT_DEVICE_ID,
             c->mqtt_host, (unsigned)c->mqtt_port);

#if ESP8266_RUN_DIAGNOSTIC
    diagnostic_mode();  /* Diagnostic mode: NEVER returns (blocks forever) */
#endif

    /* 1) Module power + basic AT OK (up to 10 retries to deal with power glitches) */
    printf("\r\nEnable ESP8266 CH_PD ......\r\n");
    macESP8266_CH_ENABLE();
    printf("\r\nAT communication test (up to 10 retries)......\r\n");
    {
        int retry = 0;
        while (!ESP8266_AT_Test()) {
            retry++;
            if (retry >= 10) {
                printf("\r\n[FATAL] AT test FAILED 10 times! Checklist:\r\n"
                         "  1) ESP8266 3P jumpers W_RX/W_TX/W_3V3 plugged correctly?\r\n"
                         "  2) ESP8266 flashed with MQTT-AT firmware (AT+GMR >= 2.2.x)?\r\n"
                         "  3) 3.3V rail stable? Add 100uF+0.1uF decoupling caps.\r\n");
                while(1) { LED1_TOGGLE; HAL_Delay(200); }
            }
            macESP8266_RST_LOW_LEVEL(); HAL_Delay(120);
            macESP8266_RST_HIGH_LEVEL(); HAL_Delay(500);
        }
    }

    /* 2) CIPMUX=0 (single connection, required for MQTT) */
    printf("\r\nSet CIPMUX=0 (single link for MQTT)......\r\n");
    while (!ESP8266_Enable_MultipleId(DISABLE));

    /* 3) STA mode + DHCP CURRENT */
    printf("\r\nSet WiFi mode = STA (client mode)......\r\n");
    while (!ESP8266_Net_Mode_Choose(STA));
    printf("\r\nEnable DHCP CURRENT ......\r\n");
    while (!ESP8266_DHCP_CUR());

    /* 4) Join WiFi（失败时 USART1 可改 SSID/密码后再试，不必烧录） */
    printf("\r\nConnect WiFi (check SSID/passwd/2.4G only!) ......\r\n");
    c = Config_Get();
    while (!ESP8266_WifiEnsure((char *)c->wifi_ssid, (char *)c->wifi_pwd)) {
        printf("[WiFi] join FAIL ssid=%s  -> USART1: config set wifi.ssid / wifi.password, config save\r\n",
               c->wifi_ssid);
        cfg_wait_window_ms(2000);
        c = Config_Get();
    }

    /* 5) MQTT layer: USERCFG -> CONNCFG(explicit keepalive/clean) -> CONN -> SUB(control+controll double-L) */
    printf("\r\nMQTT USERCFG (ClientID/username/password)......\r\n");
    while (!ESP8266_MQTT_USERCFG());

    printf("\r\nMQTT CONNCFG (explicit keepalive=60s clean=true per doc §2, 3 fallback syntax)\r\n");
    ESP8266_MQTT_CONNCFG();   /* Function does 3 retries internally, SOFT FAILS on purpose (non-critical) */

    printf("\r\nMQTT CONNECT to broker ......\r\n");
    while (!ESP8266_MQTT_CONN()) {
        printf("[MQTT] CONN FAIL  -> USART1: config set mqtt.host / mqtt.port / mqtt.user\r\n");
        /* 断网抖动后必须 CLEAN + 必要时重连 WiFi，不能盲重试 MQTTCONN */
        cfg_wait_window_ms(1500);
        mqtt_prepare_reconnect();
    }

    printf("\r\nMQTT SUBSCRIBE (double-L: control + controll, QoS=%d)......\r\n",
           MQTT_SUBSCRIBE_QOS);
    while (!ESP8266_MQTT_SUB()) {
        printf("[STARTUP] SUB stage failed. Rebuild MQTT layer before retry.\r\n");
        mqtt_prepare_reconnect();
        while (!ESP8266_MQTT_CONN()) {
            cfg_wait_window_ms(1000);
            mqtt_prepare_reconnect();
        }
        HAL_Delay(150);
    }

    MQTT_RxQueue_Reset();
    mqtt_flag = 1;
    s_last_health_ms = HAL_GetTick();
    s_last_pub_ok_ms = HAL_GetTick();

#if EEG_PROTO_ENABLE
    /* 先发网关在线（retain），再考虑 SNTP。ESP8266 MQTT-AT 上 CIPSNTPCFG
     * 打公网 NTP 会把刚连上的 WiFi 打掉，broker 只留下 LWT online:false。 */
    s_last_gw_ms = HAL_GetTick();
    EEG_PublishGatewayStatus();
#if EEG_SNTP_ENABLE
    printf("\r\nSNTP time sync (ts field needs Unix seconds)......\r\n");
    EEG_TimeInit();
    cfg_wait_window_ms(1500);
    if (!EEG_TimeSync()) {
        printf("[EEG SNTP] not synced yet - ts falls back to uptime, will retry every 60s\r\n");
    }
#else
    printf("[EEG SNTP] skipped (LAN MQTT; ts uses uptime). Set EEG_SNTP_ENABLE=1 if NTP is reachable.\r\n");
#endif
#endif

    printf("\r\nESP8266 FULL CHAIN CONFIGURED OK\r\n");
    printf("Awaiting MQTT onoff commands (value=0 start / value=2 stop)......\r\n");
    Config_SetLogVerbose(0);
    printf("[LOG] USART1 quiet  (type config show / help;  log verbose  to restore dumps)\r\n");
    LED1_OFF;   /* Red LED off = startup complete */
}

/* ================ 对外：主循环每 5s 调用（读 DHT11 + publish status） ================ */
void ESP8266_SendDHT11DataTest(void)
{
    /* CLI first: do not wait behind Modbus/MQTT dumps. */
    if (Config_CliPoll()) {
        ESP8266_ApplyNetworkConfig();
    }

    /* (1) Process MQTT downlink in main loop first, never inside USART3 ISR.
     * Evidence showed the old ISR->MQTT_RECV->PUB_REPLY path could block after
     * the first control command. */
    if (mqtt_flag) {
        static char frame[MQTT_RX_SLOT_LEN];   /* 静态：别在 1KB 主栈上摆 512 字节 */
        int budget = MQTT_RX_SLOTS;            /* 一轮最多处理一队，别饿死上报 */

        /* 取的是私有副本，Dispatch 期间 ISR 可以继续往队列里塞新帧。
         * 队列本身不受 ESP8266_ATFrame_Reset() 影响，所以派发时发 ACK
         * （上百毫秒的 AT 往返）不会再吃掉后面到达的命令。 */
        while (budget-- > 0 && MQTT_RxQueue_Pop(frame, sizeof(frame))) {
            /* 迁移期两套协议同时订阅着，由 Dispatch 按主题决定交给谁解析 */
            ESP8266_MQTT_Dispatch(frame);
        }
    }

    /* (1.5) 30s health check (called every loop entry; runs real work every 30s) */
    do_health_check();

#if EEG_PROTO_ENABLE
    /* 未对上时每分钟重试，对上后每 6h 重新校一次 */
    EEG_TimeTask();
#endif

    /* Modbus 主站轮询：刷新 g_* 缓存，MQTT 发布格式一行不动 */
    charger_poll();

    /* (2) SysTick publish_flag == 1 only every 5s AND mqtt online */
    if (!(publish_flag && mqtt_flag)) return;
    publish_flag = 0;

    /* ① DHT11：Modbus 在线时只打日志，温度改用寄存器 1039 */
    if (DHT11_Read_TempAndHumidity(&DHT11_Data) == SUCCESS) {
        if (Config_LogVerbose()) {
            printf("\r\n[DHT11] Humi=%d.%d %%RH  Temp=%d.%d C | onoff_state=%u mb=%u\r\n",
                   DHT11_Data.humi_int, DHT11_Data.humi_deci,
                   DHT11_Data.temp_int, DHT11_Data.temp_deci,
                   (unsigned)g_onoff_state,
                   charger_is_online() ? 1u : 0u);
        }
        if (!charger_is_online()) {
            g_temperature = (int16_t)DHT11_Data.temp_int;
        }
    } else if (!charger_is_online()) {
        printf("\r\n[WARN] DHT11 sample FAILED. Use last cached meter values for report.\r\n");
    }

    /* ② PUBLISH：迁移期 EEG 与旧私有协议双发，任一条失败都计入失败计数 */
    if (!charger_is_online()) {
        EEG_UpdateDerived();
    }
    {
        bool pub_ok = true;
#if EEG_PROTO_ENABLE
        if (!EEG_PublishDeviceStatus()) pub_ok = false;
        /* 10 s 原始寄存器慢通道：两次 status 发一次 */
        {
            static uint8_t s_reg_div = 0;
            if (++s_reg_div >= 2u) {
                s_reg_div = 0;
                if (!EEG_PublishRegisters()) {
                    printf("[EEG REG] skipped fail, status still counts\r\n");
                }
            }
        }
#endif
#if LEGACY_PROTO_ENABLE
        if (!ESP8266_MQTT_PUB_STATUS()) pub_ok = false;
#endif
        if (pub_ok) {
            s_pub_fail_cnt = 0;
            s_last_pub_ok_ms = HAL_GetTick();
            /* Only ever non-zero if the module out-talked the main loop and the ISR
             * had to throw bytes away - worth seeing in the serial log. */
            if (g_esp8266_rx_drop) {
                printf("[WARN] USART3 RX dropped %lu byte(s) since boot (AT buffer full)\r\n",
                       (unsigned long)g_esp8266_rx_drop);
            }
        } else {
            s_pub_fail_cnt++;
            printf("[PUBLISH] FAILED consecutive %u/%u\r\n", s_pub_fail_cnt, PUBLISH_FAIL_THRESHOLD);
            if (s_pub_fail_cnt >= PUBLISH_FAIL_THRESHOLD) {
                mqtt_flag = 0;
                rebuild_full_chain();       /* Watchdog-level full chain rebuild */
                return;
            }
        }
    }

#if EEG_PROTO_ENABLE
    /* ③ §9 过温告警（带回差，只在跨阈值时发一条） */
    EEG_CheckTempAlarm();

    /* ④ §5 网关状态心跳（比设备状态慢得多，60s 一次） */
    if ((HAL_GetTick() - s_last_gw_ms) >= EEG_GW_STATUS_INTERVAL_MS) {
        s_last_gw_ms = HAL_GetTick();
        EEG_PublishGatewayStatus();
    }
#endif
}

/************************ END OF FILE *****************************/
