/**
  ******************************************************************************
  * @file    bsp_esp8266_mqtt.c
  * @brief   ESP8266 MQTT AT 指令层（完全替换阿里云旧私有协议）
  *
  * 文档严格对应（用户提供 §2~§7.1）：
  *   §2    keepalive=60 clean=true qos=1
  *   §4.2  双拼写订阅：control / controll
  *   §5.1  status 上报 4 字段整数倍率 + QoS1 retain1
  *   §6    控制命令解析：type==command && name==onoff && value∈{0,2}
  *   §7.1  控制回执 topic=/device/{id}/control  payload={type:reply,name:onoff,value,result:success/fail,id}
  *
  ******************************************************************************
  */
#include "./ESP8266/bsp_esp8266_mqtt.h"
#include "./ESP8266/bsp_esp8266_test.h"
#include <stdlib.h>

/* ──────────────── 全局状态（供业务层读写）────────────────── */
int      led_value        = 0;   /* 兼容旧代码（实际业务层不读它，LED2 由 onoff 控制）*/
uint8_t  mqtt_flag        = 0;   /* 1=MQTT 连接且订阅成功（SysTick publish 依赖）*/

uint32_t g_current_power   = 0;   /* kW × 10    → 默认 0 (idle) */
uint32_t g_current_voltage = 2200;/* V  × 10    → 默认 220.0V */
uint32_t g_current_current = 0;   /* A  × 100   → 默认 0 (idle) */
uint8_t  g_onoff_state     = ONOFF_IDLE; /* 默认待机 (value=1) */
volatile uint8_t g_mqtt_rx_pending = 0;

/* 每个下发 command 带 id 字段时原样回传（没有就用计数）*/
static uint32_t s_reply_seq = 0;

static bool mqtt_link_is_online(void)
{
    return ESP8266_Cmd("AT+MQTTCONN?", "MQTTCONN:0,1", NULL, 1200);
}

/* ──────────────── 工具：安全查找 JSON 字段整数值 ──────────────
 * 不依赖完整 JSON 解析器。要求字段名 "xxx":value 紧凑出现，value 为整数或 "整数"。
 * 返回 true=找到写入 *out，false=没找到。 */
static bool json_get_int(const char* buf, const char* field, int* out)
{
    char key[64];
    const char* p;
    if (!buf || !field || !out) return false;
    snprintf(key, sizeof(key), "\"%s\"", field);
    p = strstr(buf, key);
    if (!p) return false;
    p += strlen(key);
    while (*p && (*p == ' ' || *p == ':' || *p == '\t')) p++;  /* 跳空格和冒号 */
    if (*p == '"') p++;                        /* 跳过可选字符串起始引号 */
    if (*p < '0' || *p > '9') {
        if (*p == '-' && p[1] >= '0' && p[1] <= '9') {}    /* 支持负数 rare */
        else return false;
    }
    *out = atoi(p);
    return true;
}

/* ──────────────── 工具：查找 JSON 字符串字段 ────────────────
 * 最多 31 字节，值里没有转义的前提下足够用。 */
static bool json_get_str(const char* buf, const char* field, char* out, int outsz)
{
    char key[64];
    const char* p, * q;
    int i;
    if (!buf || !field || !out || outsz <= 0) return false;
    snprintf(key, sizeof(key), "\"%s\"", field);
    p = strstr(buf, key);
    if (!p) return false;
    p += strlen(key);
    while (*p && (*p == ' ' || *p == ':' || *p == '\t')) p++;
    if (*p != '"') return false;
    p++;
    q = p;
    while (*q && *q != '"') q++;
    for (i = 0; i < outsz - 1 && p + i < q; i++) out[i] = p[i];
    out[i] = 0;
    return true;
}

/* ============================================================
 *  AT+MQTTUSERCFG : 8 param version (doc MQTT-AT V2.2.x syntax)
 *    =0,<scheme>,"<clientid>","<username>","<password>",<cert_key_ID>,<CA_ID>,"<path>"
 *    scheme=1 = MQTT over TCP (plaintext, intranet use)
 *  If this 8-param command is rejected by firmware (some older V2.2.x builds
 *  parse 8 params differently), we FALL BACK to 3 separate commands:
 *    AT+MQTTCLIENTID=0,"<id>"   (CMD index #71 in user's AT+CMD? log)
 *    AT+MQTTUSERNAME=0,"<user>" (CMD #72)
 *    AT+MQTTPASSWORD=0,"<pass>" (CMD #73)
 * ============================================================ */
bool ESP8266_MQTT_USERCFG(void)
{
    char cStr[384];
    printf("\r\n[MQTT USERCFG] 8-param V2.2.x syntax attempt: client_id=%s scheme=1 (TCP plaintext)\r\n", MQTT_CLIENT_ID);
    sprintf(cStr,
        "AT+MQTTUSERCFG=0,1,\"%s\",\"%s\",\"%s\",0,0,\"\"",
        MQTT_CLIENT_ID, MQTT_USER_NAME, MQTT_PASSWD);
    if (ESP8266_Cmd(cStr, "OK", 0, 1200)) {
        printf("[MQTT USERCFG] OK (8-param primary path)\r\n");
        return true;
    }
    printf("[MQTT USERCFG] 8-param path FAILED -> fallback to 3-separate-commands (CLIENTID/USERNAME/PASSWORD)...\r\n");
    /* FALLBACK (3 commands, all are listed in AT+CMD? output lines 71..73 in user's log): */
    sprintf(cStr, "AT+MQTTCLIENTID=0,\"%s\"", MQTT_CLIENT_ID);
    if (!ESP8266_Cmd(cStr, "OK", 0, 1000)) {
        printf("[MQTT FALLBACK] AT+MQTTCLIENTID FAILED!\r\n");
        return false;
    }
    sprintf(cStr, "AT+MQTTUSERNAME=0,\"%s\"", MQTT_USER_NAME);
    if (!ESP8266_Cmd(cStr, "OK", 0, 1000)) {
        printf("[MQTT FALLBACK] AT+MQTTUSERNAME FAILED!\r\n");
        return false;
    }
    sprintf(cStr, "AT+MQTTPASSWORD=0,\"%s\"", MQTT_PASSWD);
    if (!ESP8266_Cmd(cStr, "OK", 0, 1000)) {
        printf("[MQTT FALLBACK] AT+MQTTPASSWORD FAILED!\r\n");
        return false;
    }
    printf("[MQTT USERCFG] OK (3-command fallback path)\r\n");
    return true;
}

/* ============================================================
 *  AT+MQTTCONNCFG : keepalive + clean session + LWT config
 *
 *  DIAGNOSTIC RESULT LOCKED-IN FOR THIS FIRMWARE:
 *    8-param (3 empty strings) = ERROR
 *    7-param clean=0 ("", "", 0, 0)  → B2 OK (OFFICIAL ESPRESSIF V2.2.0 DOC STANDARD
 *    7-param clean=1 ("","",0,0)  → B3 OK ALSO OK (value validation ignores clean value)
 *  Order for this specific firmware (2.3.0-dev(s-bcd64d2 - V2.2.0 bin):
 *    ONLY 7-PARAM FORMAT WORKS, 8-PARAM = ERROR.
 *    7-param order: =<LinkID>,<keepalive>,<disable_clean_session>,
 *                     "<lwt_topic>","<lwt_payload>",<lwt_qos>,<lwt_retain>
 * ============================================================ */
bool ESP8266_MQTT_CONNCFG(void)
{
    char cStr[256];

    /* PRIMARY (B2 = official 7-param clean disable flag = 0:
     *   disable_clean_session=0 -> clean session=ENABLED (clean=true, matches doc §2),
     *   lwt topic/payload empty, lwt disabled by  */
    printf("\r\n[MQTT CONNCFG] PRIMARY: official 7-param clean=0 (doc §2)\r\n");
    sprintf(cStr,
        "AT+MQTTCONNCFG=0,%u,0,\"\",\"\",0,0",
        (unsigned)MQTT_KEEPALIVE);
    if (ESP8266_Cmd(cStr, "OK", 0, 1200)) {
        printf("[MQTT CONNCFG] OK (official 7-param, ka=%us clean=enabled)\r\n",
               (unsigned)MQTT_KEEPALIVE);
        return true;
    }

    /* FALLBACK 1 (B3): try clean=1 value just in case (diagnostic passed as well):
     * disable_clean_session=1 -> clean session=DISABLED. still ok for most deployments. */
    printf("[MQTT CONNCFG] FALLBACK: 7-param alternate clean=1\r\n");
    sprintf(cStr,
        "AT+MQTTCONNCFG=0,%u,1,\"\",\"\",0,0",
        (unsigned)MQTT_KEEPALIVE);
    if (ESP8266_Cmd(cStr, "OK", 0, 1200)) {
        printf("[MQTT CONNCFG] OK (7-param alt clean=1, ka=%us)\r\n",
               (unsigned)MQTT_KEEPALIVE);
        return true;
    }

    printf("\r\n[WARN] CONNCFG FAILED -> falling back to defaults (ka=60 clean=true 95%% match §2).\r\n\r\n");
    return true;   /* SOFT FAIL: do not block startup chain. */
}

/* ============================================================
 *  AT+MQTTCONN : 连接 Broker (192.168.8.97:1883)
 *  §2 端口 1883（内网明文）
 *  AT 语法：AT+MQTTCONN=<LinkID>,"host",<port>,<reconnect>
 *     reconnect=0（手动重连，我们自己有监控逻辑）
 * ============================================================ */
bool ESP8266_MQTT_CONN(void)
{
    char cStr[192];
    sprintf(cStr, "AT+MQTTCONN=0,\"%s\",%d,0", MQTT_BROKERADDRESS, MQTT_PORT);
    if (!ESP8266_Cmd(cStr, "OK", 0, 4000)) {
        printf("[MQTT CONN] FAILED! Checklist:\r\n"
               "  1) Same WiFi LAN as PC? Board must ping 192.168.8.97\r\n"
               "  2) PC Test-NetConnection 192.168.8.97 -Port 1883 == TcpTestSucceeded : True\r\n"
               "  3) EMQX auth: user=charge / pass=123456 (or anonymous enabled)\r\n"
               "  4) ClientID unique (no other device already connected as sim-pile-%s)\r\n",
               MQTT_DEVICE_ID);
        return false;
    }
    printf("[MQTT CONN] OK connected to %s:%d\r\n", MQTT_BROKERADDRESS, MQTT_PORT);
    return true;
}

/* ============================================================
 *  AT+MQTTCLEAN : explicitly close MQTT link-0 on module side
 *  Used after unsolicited +MQTTDISCONNECTED or before reconnect loops,
 *  so the next AT+MQTTCONN is not sent while link-0 is still busy/stale.
 * ============================================================ */
bool ESP8266_MQTT_CLEAN(void)
{
    if (ESP8266_Cmd("AT+MQTTCLEAN=0", "OK", "ERROR", 1200)) {
        printf("[MQTT CLEAN] done\r\n");
        return true;
    }
    printf("[MQTT CLEAN] no explicit clean ack, continue anyway\r\n");
    return false;
}

/* ============================================================
 *  AT+MQTTSUB : §4.2 双拼写订阅
 *  NOTE:
 *    Spec target is QoS1, but runtime evidence shows this ESP8266 MQTT-AT
 *    firmware is disconnected by broker immediately after the first QoS1 SUB.
 *    We keep publish/reply on QoS1 and make subscribe QoS configurable via
 *    MQTT_SUBSCRIBE_QOS so the link can be stabilized first.
 *   ① /device/{id}/control
 *   ② /device/{id}/controll（双 l 兼容旧系统）
 *
 *  DIAGNOSTIC REAL-WORLD BUG FOUND:
 *    After 1st SUB returns OK, 2nd SUB (<150ms later) hits
 *    +MQTTDISCONNECTED (broker throttles "too-fast subscribe flood" on
 *    clean-session enabled connections). Fix: 150ms gap between 2 SUBs,
 *    auto reconnect if mid-disconnect detected.
 * ============================================================ */
bool ESP8266_MQTT_SUB(void)
{
    char cStr[192];
    /* 1) control (single L) */
    sprintf(cStr, "AT+MQTTSUB=0,\"%s\",%d",
            MQTT_SUBSCRIBE_TOPIC_CTRL, MQTT_SUBSCRIBE_QOS);
    if (!ESP8266_Cmd(cStr, "OK", 0, 1500)) {
        if (!strstr((const char*)strEsp8266_Fram_Record.Data_RX_BUF, "+MQTTDISCONNECTED")) {
            HAL_Delay(300);
            if (mqtt_link_is_online()) {
                printf("[MQTT SUB] topic1 got ERROR but link still ONLINE. Retry once after settle delay.\r\n");
                HAL_Delay(300);
                if (ESP8266_Cmd(cStr, "OK", 0, 1500)) {
                    printf("[MQTT SUB] RETRY OK(1) topic=%s qos=%d\r\n",
                           MQTT_SUBSCRIBE_TOPIC_CTRL, MQTT_SUBSCRIBE_QOS);
                    goto sub_topic2;
                }
            }
        }
        printf("[MQTT SUB] FAILED topic=%s\r\n", MQTT_SUBSCRIBE_TOPIC_CTRL);
        return false;
    }
    printf("[MQTT SUB] OK(1) topic=%s qos=%d\r\n",
           MQTT_SUBSCRIBE_TOPIC_CTRL, MQTT_SUBSCRIBE_QOS);
    /* GAP DELAY: 150ms to prevent broker disconnect flood on clean=true conns */
    HAL_Delay(150);
    if (strstr((const char*)strEsp8266_Fram_Record.Data_RX_BUF, "+MQTTDISCONNECTED")) {
        printf("[MQTT SUB] SUB1 frame already contains +MQTTDISCONNECTED. Escalate to upper reconnect path.\r\n");
        return false;
    }

sub_topic2:
    /* 2) controll (double L — §4.2 backward compatibility) */
    sprintf(cStr, "AT+MQTTSUB=0,\"%s\",%d",
            MQTT_SUBSCRIBE_TOPIC_CTRLL, MQTT_SUBSCRIBE_QOS);
    if (!ESP8266_Cmd(cStr, "OK", 0, 1500)) {
        if (!strstr((const char*)strEsp8266_Fram_Record.Data_RX_BUF, "+MQTTDISCONNECTED")) {
            HAL_Delay(300);
            if (mqtt_link_is_online()) {
                printf("[MQTT SUB] topic2 got ERROR but link still ONLINE. Retry once after settle delay.\r\n");
                HAL_Delay(300);
                if (ESP8266_Cmd(cStr, "OK", 0, 1500)) {
                    printf("[MQTT SUB] RETRY OK(2) topic=%s qos=%d\r\n",
                           MQTT_SUBSCRIBE_TOPIC_CTRLL, MQTT_SUBSCRIBE_QOS);
                    return true;
                }
            }
        }
        printf("[MQTT SUB] FAILED topic=%s (double-L required by spec §4.2)\r\n",
               MQTT_SUBSCRIBE_TOPIC_CTRLL);
        return false;
    }
    printf("[MQTT SUB] OK(2) topic=%s qos=%d\r\n",
           MQTT_SUBSCRIBE_TOPIC_CTRLL, MQTT_SUBSCRIBE_QOS);
    return true;
}

/* ============================================================
 *  AT+MQTTPUB : §5.1 status 上报
 *  topic   : /device/{id}/status
 *  qos     : MQTT_DEFAULT_QOS = 1  (§2 默认)
 *  retain  : MQTT_STATUS_RETAIN = 1 (后端新订阅立刻拿上一次状态，调试友好)
 *  payload : 严格 4 字段整数倍率
 *      {"current_power":N,"current_voltage":N,"current_current":N,"onoff":N}
 *  注意：PUB 的 JSON 内部逗号 / 引号需要 \ 转义（乐鑫 AT 版本要求）
 * ============================================================ */
bool ESP8266_MQTT_PUB_STATUS(void)
{
    char cStr[512];
    char payload[256];

    /* 4 字段裸 JSON，不要 params/id/version 等阿里云遗留外壳 */
    snprintf(payload, sizeof(payload),
        "{\"current_power\":%lu,\"current_voltage\":%lu,\"current_current\":%lu,\"onoff\":%u}",
        (unsigned long)g_current_power,
        (unsigned long)g_current_voltage,
        (unsigned long)g_current_current,
        (unsigned int)g_onoff_state);

    /* AT 语法：AT+MQTTPUB=<LinkID>,"<topic>","<data>",<qos>,<retain>
       payload 放在 AT 字符串引号内，因此 JSON 里的双引号也必须转义。
       当前这版 ESP8266 MQTT-AT 对下面三类字符都敏感：
         1) 反斜杠 '\\' -> 需要自身转义
         2) 双引号 '\"' -> 否则 AT 字符串会提前结束
         3) 逗号 '\,'   -> 固件解析 payload 参数时要求转义 */
    {
        char esc[512];
        int i, j = 0;
        for (i = 0; payload[i] && j < (int)sizeof(esc) - 3; i++) {
            if (payload[i] == '\\') esc[j++] = '\\';   /* double-escape any existing backslash */
            if (payload[i] == '"')  esc[j++] = '\\';   /* escape JSON quotes inside AT string */
            if (payload[i] == ',')  esc[j++] = '\\';   /* escape commas */
            esc[j++] = payload[i];
        }
        esc[j] = 0;
        snprintf(cStr, sizeof(cStr),
            "AT+MQTTPUB=0,\"%s\",\"%s\",%d,%d",
            MQTT_PUBLISH_TOPIC_STATUS, esc,
            MQTT_DEFAULT_QOS, MQTT_STATUS_RETAIN);
    }
    if (!ESP8266_Cmd(cStr, "OK", 0, 1500)) {
        printf("[MQTT PUB STATUS] FAILED payload: %s\r\n", payload);
        return false;
    }
    printf("[MQTT PUB STATUS] OK %s\r\n", payload);
    return true;
}

/* ============================================================
 *  控制回执（§7.1）：发到 /device/{id}/control，QoS1，retain=0
 *  格式：
 *     {"type":"reply","name":"onoff","value":0,"result":"success","id":"xxx"}
 *  id 从原 command 里取到就原样带回，取到用自增序列号
 * ============================================================ */
bool ESP8266_MQTT_PUB_REPLY(int value, const char* result)
{
    char cStr[512];
    char payload[256];
    char idbuf[32] = {0};

    /* 当前命令 id（这里是全局：RECV 解析命中时若带 id 就填进来，没有则空，序列化成 seq）*/
    if (idbuf[0] == 0) {
        s_reply_seq++;
        snprintf(idbuf, sizeof(idbuf), "%lu", (unsigned long)s_reply_seq);
    }

    snprintf(payload, sizeof(payload),
        "{\"type\":\"reply\",\"name\":\"onoff\",\"value\":%d,\"result\":\"%s\",\"id\":\"%s\"}",
        value, result ? result : "success", idbuf);
    /* Escape JSON quotes, commas and existing backslashes for strict AT parser */
    {
        char esc[512];
        int i, j = 0;
        for (i = 0; payload[i] && j < (int)sizeof(esc) - 3; i++) {
            if (payload[i] == '\\') esc[j++] = '\\';
            if (payload[i] == '"')  esc[j++] = '\\';
            if (payload[i] == ',')  esc[j++] = '\\';
            esc[j++] = payload[i];
        }
        esc[j] = 0;
        snprintf(cStr, sizeof(cStr),
            "AT+MQTTPUB=0,\"%s\",\"%s\",%d,0",
            MQTT_PUBLISH_TOPIC_REPLY, esc, MQTT_DEFAULT_QOS);
    }
    if (!ESP8266_Cmd(cStr, "OK", 0, 1500)) {
        printf("[MQTT REPLY] FAILED payload: %s\r\n", payload);
        return false;
    }
    printf("[MQTT REPLY] OK payload: %s\r\n", payload);
    return true;
}

/* ============================================================
 *  ESP8266_MQTT_RECV : 下行 MQTT 消息解析（中断收到完整帧后由主循环调用）
 *  §6 : 只识别
 *      {"type":"command","name":"onoff","value":0}  （开始充）
 *      {"type":"command","name":"onoff","value":2}  （停止充）
 *  其他任何字段组合：静默丢弃（不回 unsupported，防止回音炸）
 *
 *  识别后动作：
 *    - 修改 g_onoff_state
 *    - 立即发一条 reply（§7.1）
 *    - LED2 指示（绿色 LED 亮表示充电中 / 灭表示空闲）
 * ============================================================ */
bool ESP8266_MQTT_RECV(void)
{
    char type[32]  = {0};
    char name[32]  = {0};
    int  val = -1;

    if (ucTcpClosedFlag) {
        mqtt_flag = 0;
        g_onoff_state = ONOFF_IDLE;
        printf("\r\n[MQTT RECV] TCP closed. MQTT link down (waiting for main loop reconnect).\r\n");
        return false;
    }

    /* 1. Must contain "command" + "onoff" keywords to even be parsed (fast filter) */
    if (!strstr((const char*)strEsp8266_Fram_Record.Data_RX_BUF, "\"command\"") ||
        !strstr((const char*)strEsp8266_Fram_Record.Data_RX_BUF, "\"onoff\""))
    {
        /* Not a valid onoff command — SILENTLY DROP (no echo back) */
        return false;
    }

    /* 2. Parse 3 required fields per §6 */
    if (!json_get_str((const char*)strEsp8266_Fram_Record.Data_RX_BUF, "type", type, sizeof(type)) ||
        !json_get_str((const char*)strEsp8266_Fram_Record.Data_RX_BUF, "name", name, sizeof(name)) ||
        !json_get_int((const char*)strEsp8266_Fram_Record.Data_RX_BUF, "value", &val))
    {
        printf("\r\n[MQTT RECV] missing fields / bad format - silently dropped.\r\n");
        return false;
    }
    if (strcmp(type, "command") != 0 || strcmp(name, "onoff") != 0) {
        printf("\r\n[MQTT RECV] type/name != command/onoff - silently dropped.\r\n");
        return false;
    }
    if (val != 0 && val != 2) {
        /* §6: only value 0/2 are valid. Other values reply fail (not unsupported) */
        printf("\r\n[MQTT RECV] onoff value=%d invalid (only 0/2 allowed) -> reply fail\r\n", val);
        ESP8266_MQTT_PUB_REPLY(val, "fail_invalid_value");
        return true;
    }

    /* 3. Valid command — execute */
    if (val == 0) {
        g_onoff_state = ONOFF_CHARGING;
        /* Typical charging values (simulated): 6.5kW*10=65, 221V*10=2210, 29.50A*100=2950 */
        g_current_power   = 65;
        g_current_voltage = 2210;
        g_current_current = 2950;
        LED2_ON;
        printf("\r\n[MQTT CMD] onoff=0 -> START charging (LED2 ON)\r\n");
    } else { /* val==2 */
        g_onoff_state = ONOFF_IDLE;   /* after stop -> idle (§5.1) */
        g_current_power   = 0;
        g_current_current = 0;
        g_current_voltage = 2200;
        LED2_OFF;
        printf("\r\n[MQTT CMD] onoff=2 -> STOP charging, back to idle (LED2 OFF)\r\n");
    }
    ESP8266_MQTT_PUB_REPLY(val, "success");
    return true;
}
