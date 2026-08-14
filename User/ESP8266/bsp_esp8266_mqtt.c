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
#include "./ESP8266/bsp_eeg_proto.h"
#include <stdlib.h>

#if !EEG_PROTO_ENABLE && !LEGACY_PROTO_ENABLE
#error "EEG_PROTO_ENABLE and LEGACY_PROTO_ENABLE cannot both be 0: nothing would be published or subscribed."
#endif

/* ──────────────── 全局状态（供业务层读写）────────────────── */
int      led_value        = 0;   /* 兼容旧代码（实际业务层不读它，LED2 由 onoff 控制）*/
uint8_t  mqtt_flag        = 0;   /* 1=MQTT 连接且订阅成功（SysTick publish 依赖）*/

uint32_t g_current_power   = 0;   /* kW × 10    → 默认 0 (idle) */
uint32_t g_current_voltage = 2200;/* V  × 10    → 默认 220.0V */
uint32_t g_current_current = 0;   /* A  × 100   → 默认 0 (idle) */
uint8_t  g_onoff_state     = ONOFF_IDLE; /* 默认待机 (value=1) */

/* 每个下发 command 带 id 字段时原样回传（没有就用计数）*/
static uint32_t s_reply_seq = 0;
static char     s_reply_id[32] = {0};

/* ══════════════════ 下行帧队列 ══════════════════════════════════
 *  ISR 只写 s_rx_head，主循环只写 s_rx_tail，单生产者单消费者，
 *  因此不需要临界区；填完槽位再推进 head，读端就永远看不到半截帧。
 * ══════════════════════════════════════════════════════════════ */
#define URC_TAG      "+MQTTSUBRECV"
#define URC_TAG_LEN  12                     /* strlen(URC_TAG) */
#define URC_INCOMPLETE   0u                 /* 帧还没收全，下次 IDLE 再看 */
#define URC_MALFORMED    0xFFFFu            /* 头部对不上，跳过这个 tag */

static char s_rx_slot[MQTT_RX_SLOTS][MQTT_RX_SLOT_LEN];
static volatile uint8_t s_rx_head = 0;      /* 只由 ISR 推进 */
static volatile uint8_t s_rx_tail = 0;      /* 只由主循环推进 */
static uint16_t s_urc_scan = 0;             /* AT 缓冲区里已切过帧的水位 */

volatile uint32_t g_mqtt_rx_dropped = 0;

static uint8_t rx_next(uint8_t i)
{
    return (uint8_t)((i + 1u) % MQTT_RX_SLOTS);
}

/* 有界子串查找：URC 只在缓冲区末尾有 '\0'，用 strstr 会越过本帧读到下一帧 */
static bool mem_has(const char* hay, uint16_t hlen, const char* needle)
{
    uint16_t nlen = (uint16_t)strlen(needle);
    uint16_t i;

    if (nlen == 0 || hlen < nlen) return false;
    for (i = 0; i <= (uint16_t)(hlen - nlen); i++) {
        if (memcmp(hay + i, needle, nlen) == 0) return true;
    }
    return false;
}

static const char* mem_find_tag(const char* hay, uint16_t hlen)
{
    uint16_t i;

    if (hlen < URC_TAG_LEN) return NULL;
    for (i = 0; i <= (uint16_t)(hlen - URC_TAG_LEN); i++) {
        if (memcmp(hay + i, URC_TAG, URC_TAG_LEN) == 0) return hay + i;
    }
    return NULL;
}

/* 按 URC 自带的长度字段算出整帧边界，而不是猜一个 '}'：
 *   +MQTTSUBRECV:<LinkID>,"<topic>",<LengthOfData>,<data>
 * payload 里带花括号或逗号时，只有长度字段是可靠的。*/
static uint16_t urc_frame_len(const char* p, uint16_t avail)
{
    uint16_t i = URC_TAG_LEN;
    uint32_t dlen = 0;
    int digits = 0;

    if (i >= avail) return URC_INCOMPLETE;
    if (p[i] != ':') return URC_MALFORMED;
    i++;

    while (i < avail && p[i] != ',') i++;               /* LinkID */
    if (i >= avail) return URC_INCOMPLETE;
    i++;

    if (i >= avail) return URC_INCOMPLETE;
    if (p[i] != '"') return URC_MALFORMED;
    i++;
    while (i < avail && p[i] != '"') i++;               /* topic */
    if (i >= avail) return URC_INCOMPLETE;
    i++;

    if (i >= avail) return URC_INCOMPLETE;
    if (p[i] != ',') return URC_MALFORMED;
    i++;

    while (i < avail && p[i] >= '0' && p[i] <= '9') {
        dlen = dlen * 10u + (uint32_t)(p[i] - '0');
        if (dlen >= MQTT_RX_SLOT_LEN) return URC_MALFORMED;  /* 再等也放不下 */
        i++;
        digits++;
    }
    if (i >= avail) return URC_INCOMPLETE;
    if (digits == 0 || p[i] != ',') return URC_MALFORMED;
    i++;

    if ((uint32_t)i + dlen > (uint32_t)avail) return URC_INCOMPLETE;
    return (uint16_t)(i + dlen);
}

static void rx_push(const char* urc, uint16_t len)
{
    uint8_t head = s_rx_head;
    uint8_t nxt  = rx_next(head);

    if (len == 0 || len >= MQTT_RX_SLOT_LEN || nxt == s_rx_tail) {
        g_mqtt_rx_dropped++;                /* 队列满或帧过长：丢弃并计数 */
        return;
    }
    memcpy(s_rx_slot[head], urc, len);
    s_rx_slot[head][len] = '\0';
    __DMB();                                /* 槽位写完再对读端可见 */
    s_rx_head = nxt;
}

/*
 * 把 AT 缓冲区里所有"完整且不是自己回声"的 +MQTTSUBRECV 切出来入队。
 * 只在两处调用，都保证不会重入：USART3 的 IDLE 中断里，以及
 * ESP8266_ATFrame_Reset() 关中断清缓冲之前（先存后清，避免正要发下一条
 * AT 命令时把已经收全的下行帧一起冲掉）。
 */
void MQTT_RxQueue_Harvest(void)
{
    const char* buf   = (const char*)strEsp8266_Fram_Record.Data_RX_BUF;
    uint16_t    total = strEsp8266_Fram_Record.InfBit.FramLength;

    if (mqtt_flag != 1) return;
    if (s_urc_scan > total) s_urc_scan = 0;   /* 缓冲区在背后被清过 */

    while (s_urc_scan < total) {
        const char* p = mem_find_tag(buf + s_urc_scan, (uint16_t)(total - s_urc_scan));
        uint16_t off, flen;

        /* 没找到 tag：把水位推到"末尾往回退一个 tag 长度"处。
         * 退这一截是为了接住跨在本次帧尾的半个 "+MQTTSUB"；不推进的话，
         * 每次 IDLE 都要把整个 2KB 缓冲重扫一遍，中断里 ~140us，
         * 已经接近 115200 下一个字节的时间，会开始丢字节。*/
        if (p == NULL) {
            uint16_t keep = (uint16_t)(URC_TAG_LEN - 1);
            uint16_t back = (total > keep) ? (uint16_t)(total - keep) : 0;
            if (back > s_urc_scan) s_urc_scan = back;
            return;
        }

        off  = (uint16_t)(p - buf);
        flen = urc_frame_len(p, (uint16_t)(total - off));

        if (flen == URC_INCOMPLETE) {
            s_urc_scan = off;                 /* 收全了再回来切 */
            return;
        }
        if (flen == URC_MALFORMED) {
            s_urc_scan = (uint16_t)(off + URC_TAG_LEN);
            continue;
        }

        /* 旧协议把回执发到自己订阅的主题上，回声会原样绕回来。
         * EEG 没这问题（ACK 发到 .../ack，没人订阅）。*/
        if (!mem_has(p, flen, "\"type\":\"reply\"")) {
            rx_push(p, flen);
        }
        s_urc_scan = (uint16_t)(off + flen);
    }
}

bool MQTT_RxQueue_Pop(char* out, int outsz)
{
    uint8_t  tail = s_rx_tail;
    uint16_t len;

    if (out == NULL || outsz <= 0) return false;
    if (tail == s_rx_head) return false;

    len = (uint16_t)strlen(s_rx_slot[tail]);
    if (len > (uint16_t)(outsz - 1)) len = (uint16_t)(outsz - 1);
    memcpy(out, s_rx_slot[tail], len);
    out[len] = '\0';

    __DMB();
    s_rx_tail = rx_next(tail);
    return true;
}

void MQTT_RxScan_Reset(void)
{
    s_urc_scan = 0;
}

void MQTT_RxQueue_Reset(void)
{
    /* 从主循环调用，而 head/水位归 ISR 写，所以这里要屏蔽中断 */
    uint32_t primask = __get_PRIMASK();

    __disable_irq();
    s_rx_tail  = s_rx_head;
    s_urc_scan = 0;
    __set_PRIMASK(primask);
}

static bool mqtt_link_is_online(void)
{
    return ESP8266_Cmd("AT+MQTTCONN?", "MQTTCONN:0,1", NULL, 1200);
}

/* ──────────────── 工具：安全查找 JSON 字段整数值 ──────────────
 * 不依赖完整 JSON 解析器。要求字段名 "xxx":value 紧凑出现，value 为整数或 "整数"。
 * 返回 true=找到写入 *out，false=没找到。 */
bool MQTT_JsonGetInt(const char* buf, const char* field, int* out)
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
bool MQTT_JsonGetStr(const char* buf, const char* field, char* out, int outsz)
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

/* 读取 JSON 字段原始文本，支持：
 *   "id":"abc"
 *   "id":1001
 * 返回值统一写入 out，便于 reply 原样带回。 */
bool MQTT_JsonGetText(const char* buf, const char* field, char* out, int outsz)
{
    char key[64];
    const char* p;
    const char* q;
    int i = 0;

    if (!buf || !field || !out || outsz <= 1) return false;
    snprintf(key, sizeof(key), "\"%s\"", field);
    p = strstr(buf, key);
    if (!p) return false;
    p += strlen(key);
    while (*p && (*p == ' ' || *p == ':' || *p == '\t')) p++;

    if (*p == '"') {
        p++;
        q = p;
        while (*q && *q != '"') q++;
    } else {
        q = p;
        while (*q && *q != ',' && *q != '}' && *q != ' ' && *q != '\r' && *q != '\n' && *q != '\t') q++;
    }

    if (q <= p) return false;
    while (i < outsz - 1 && p + i < q) {
        out[i] = p[i];
        i++;
    }
    out[i] = 0;
    return i > 0;
}

/* ──────────────── 发布层：AT 命令行长度自适应 ────────────────
 * 这两块缓冲故意做成 static：整条 AT 命令最长可到 700+ 字节，而工程栈只有
 * 2KB（startup_stm32f103xe.s），printf/中断还要在同一个栈上嵌套，放栈上迟早出事。
 * 这里不可重入，但发布本来就只在主循环里串行调用。 */
static char s_at_cmd[768];
static char s_at_esc[512];

/* ESP-AT 把 payload 塞在 AT 命令行的引号里，反斜杠 / 双引号 / 逗号都必须转义，
 * 否则参数会被提前截断。返回写入长度。 */
static int at_escape(const char* src, char* out, int outsz)
{
    int i, j = 0;
    for (i = 0; src[i]; i++) {
        if (j >= outsz - 3) break;
        if (src[i] == '\\' || src[i] == '"' || src[i] == ',') out[j++] = '\\';
        out[j++] = src[i];
    }
    out[j] = 0;
    return j;
}

/* ============================================================
 *  统一发布入口：短报文走 AT+MQTTPUB，长报文自动切 AT+MQTTPUBRAW
 *
 *  乐鑫的硬限制是「整条 AT 命令必须小于 256 字节」（ESP-AT 文档 AT+MQTTPUB
 *  的 Notes）。要命的是转义：EEG §6 那条 171 字节的设备状态 JSON，光是 24 个
 *  引号加 10 个逗号就要多出 34 字节，连上主题和命令头一共 266 字节，正好越界。
 *
 *  PUBRAW 是两段式：先发长度，模块回 '>' 之后把原始字节整段推过去，
 *  不需要任何转义，长度只受模块内存限制。本机固件支持（AT+CMD? 里 #77）。
 * ============================================================ */
bool ESP8266_MQTT_Publish(const char* topic, const char* payload, int qos, int retain)
{
    int len = (int)strlen(payload);
    int n;

    at_escape(payload, s_at_esc, sizeof(s_at_esc));
    n = snprintf(s_at_cmd, sizeof(s_at_cmd), "AT+MQTTPUB=0,\"%s\",\"%s\",%d,%d",
                 topic, s_at_esc, qos, retain);

    if (n > 0 && n < ESP8266_AT_CMD_LIMIT && n < (int)sizeof(s_at_cmd)) {
        return ESP8266_Cmd(s_at_cmd, "OK", 0, 3000);
    }

    {
        /* 只提示一次，免得每 5s 刷一行 */
        static bool raw_notified = false;
        if (!raw_notified) {
            raw_notified = true;
            printf("[MQTT] AT+MQTTPUB would be %d bytes (limit %d) -> switching to AT+MQTTPUBRAW\r\n",
                   n, ESP8266_AT_CMD_LIMIT);
        }
    }

    snprintf(s_at_cmd, sizeof(s_at_cmd), "AT+MQTTPUBRAW=0,\"%s\",%d,%d,%d",
             topic, len, qos, retain);
    /* 只发不等：紧接着自己等 '>'，这样 ERROR 能立刻退出而不是干等超时 */
    ESP8266_Cmd(s_at_cmd, 0, 0, 0);
    if (!ESP8266_AT_WaitFor(">", "ERROR", 3000)) {
        printf("[MQTT PUBRAW] no '>' prompt (topic=%s len=%d)\r\n", topic, len);
        return false;
    }

    ESP8266_SendRaw(payload, (uint16_t)len);
    if (!ESP8266_AT_WaitFor("+MQTTPUB:OK", "+MQTTPUB:FAIL", 8000)) {
        printf("[MQTT PUBRAW] FAILED (topic=%s len=%d)\r\n", topic, len);
        return false;
    }
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

#if EEG_PROTO_ENABLE
    /* §5 遗嘱：网关掉线时由 broker 代发 offline，retain=1 让后来的订阅者也能看到。
     * ts 写 0 是有意的——遗嘱是连接时就交给 broker 的，模块死掉那一刻没法再改
     * 里面的时间戳，消费侧应该以 broker 收到的时间为准。
     * 注意 lwt_topic / lwt_msg 各自上限 128 字节，且整条命令同样受 256 字节约束；
     * 这里算下来约 100 字节，安全。 */
    at_escape("{\"online\":false,\"ts\":0}", s_at_esc, sizeof(s_at_esc));
    printf("\r\n[MQTT CONNCFG] PRIMARY: 7-param clean=0 + LWT (doc §2/§5)\r\n");
    snprintf(cStr, sizeof(cStr),
        "AT+MQTTCONNCFG=0,%u,0,\"%s\",\"%s\",%d,%d",
        (unsigned)MQTT_KEEPALIVE, EEG_TOPIC_GW_STATUS, s_at_esc,
        MQTT_DEFAULT_QOS, EEG_RETAIN_STATUS);
    if (ESP8266_Cmd(cStr, "OK", 0, 1200)) {
        printf("[MQTT CONNCFG] OK (ka=%us clean=enabled, LWT -> %s)\r\n",
               (unsigned)MQTT_KEEPALIVE, EEG_TOPIC_GW_STATUS);
        return true;
    }
    printf("[MQTT CONNCFG] LWT variant rejected -> retry without LWT\r\n");
#endif

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
static bool mqtt_sub_one(const char* topic)
{
    char cStr[192];

    snprintf(cStr, sizeof(cStr), "AT+MQTTSUB=0,\"%s\",%d", topic, MQTT_SUBSCRIBE_QOS);

    if (ESP8266_Cmd(cStr, "OK", 0, 1500)) {
        printf("[MQTT SUB] OK topic=%s qos=%d\r\n", topic, MQTT_SUBSCRIBE_QOS);
        return true;
    }
    /* 重复订阅同一主题固件回 "ALREADY SUBSCRIBE"，不是失败（快速重连会碰到） */
    if (strstr((const char*)strEsp8266_Fram_Record.Data_RX_BUF, "ALREADY SUBSCRIBE")) {
        printf("[MQTT SUB] already subscribed topic=%s\r\n", topic);
        return true;
    }
    if (!strstr((const char*)strEsp8266_Fram_Record.Data_RX_BUF, "+MQTTDISCONNECTED")) {
        HAL_Delay(300);
        if (mqtt_link_is_online()) {
            printf("[MQTT SUB] topic=%s got ERROR but link still ONLINE. Retry once after settle delay.\r\n",
                   topic);
            HAL_Delay(300);
            if (ESP8266_Cmd(cStr, "OK", 0, 1500)) {
                printf("[MQTT SUB] RETRY OK topic=%s qos=%d\r\n", topic, MQTT_SUBSCRIBE_QOS);
                return true;
            }
        }
    }
    printf("[MQTT SUB] FAILED topic=%s\r\n", topic);
    return false;
}

bool ESP8266_MQTT_SUB(void)
{
    static const char* const topics[] = {
#if EEG_PROTO_ENABLE
        EEG_TOPIC_DEV_CMD,              /* EEG V1.0 §7 控制命令 */
#endif
#if LEGACY_PROTO_ENABLE
        MQTT_SUBSCRIBE_TOPIC_CTRL,      /* 旧私有协议 §4.2 control  */
        MQTT_SUBSCRIBE_TOPIC_CTRLL,     /* 旧私有协议 §4.2 controll（双 l） */
#endif
    };
    const int topic_cnt = (int)(sizeof(topics) / sizeof(topics[0]));
    int i;

    for (i = 0; i < topic_cnt; i++) {
        if (i > 0) {
            /* 两条 SUB 之间必须留间隔：实测间隔 <150ms 连发时，broker 会把它
             * 当成 subscribe flood，在 clean-session 连接上直接回
             * +MQTTDISCONNECTED 把链路掐掉。 */
            HAL_Delay(150);
            if (strstr((const char*)strEsp8266_Fram_Record.Data_RX_BUF, "+MQTTDISCONNECTED")) {
                printf("[MQTT SUB] link dropped mid-subscribe. Escalate to upper reconnect path.\r\n");
                return false;
            }
        }
        if (!mqtt_sub_one(topics[i])) {
            return false;
        }
    }
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
    static char payload[256];

    /* 4 字段裸 JSON，不要 params/id/version 等阿里云遗留外壳 */
    snprintf(payload, sizeof(payload),
        "{\"current_power\":%lu,\"current_voltage\":%lu,\"current_current\":%lu,\"onoff\":%u}",
        (unsigned long)g_current_power,
        (unsigned long)g_current_voltage,
        (unsigned long)g_current_current,
        (unsigned int)g_onoff_state);

    if (!ESP8266_MQTT_Publish(MQTT_PUBLISH_TOPIC_STATUS, payload,
                              MQTT_DEFAULT_QOS, MQTT_STATUS_RETAIN)) {
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
    static char payload[256];
    char idbuf[32] = {0};

    /* 当前命令 id（这里是全局：RECV 解析命中时若带 id 就填进来，没有则空，序列化成 seq）*/
    if (s_reply_id[0] != 0) {
        snprintf(idbuf, sizeof(idbuf), "%s", s_reply_id);
    } else {
        s_reply_seq++;
        snprintf(idbuf, sizeof(idbuf), "%lu", (unsigned long)s_reply_seq);
    }

    snprintf(payload, sizeof(payload),
        "{\"type\":\"reply\",\"name\":\"onoff\",\"value\":%d,\"result\":\"%s\",\"id\":\"%s\"}",
        value, result ? result : "success", idbuf);

    if (!ESP8266_MQTT_Publish(MQTT_PUBLISH_TOPIC_REPLY, payload, MQTT_DEFAULT_QOS, 0)) {
        s_reply_id[0] = 0;
        printf("[MQTT REPLY] FAILED payload: %s\r\n", payload);
        return false;
    }
    s_reply_id[0] = 0;
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
bool ESP8266_MQTT_RECV(const char* frame)
{
    const char* rx_buf = frame;
    char type[32]  = {0};
    char name[32]  = {0};
    char cmd_id[32] = {0};
    int  val = -1;

    if (rx_buf == NULL) return false;

    if (ucTcpClosedFlag) {
        mqtt_flag = 0;
        g_onoff_state = ONOFF_IDLE;
        printf("\r\n[MQTT RECV] TCP closed. MQTT link down (waiting for main loop reconnect).\r\n");
        return false;
    }

    /* 1. Must contain "command" + "onoff" keywords to even be parsed (fast filter) */
    if (!strstr(rx_buf, "\"command\"") ||
        !strstr(rx_buf, "\"onoff\""))
    {
        /* Not a valid onoff command — SILENTLY DROP (no echo back) */
        return false;
    }

    /* 2. Parse 3 required fields per §6 */
    if (!MQTT_JsonGetStr(rx_buf, "type", type, sizeof(type)) ||
        !MQTT_JsonGetStr(rx_buf, "name", name, sizeof(name)) ||
        !MQTT_JsonGetInt(rx_buf, "value", &val))
    {
        printf("\r\n[MQTT RECV] missing fields / bad format - silently dropped.\r\n");
        return false;
    }
    if (strcmp(type, "command") != 0 || strcmp(name, "onoff") != 0) {
        printf("\r\n[MQTT RECV] type/name != command/onoff - silently dropped.\r\n");
        return false;
    }
    if (MQTT_JsonGetText(rx_buf, "id", cmd_id, sizeof(cmd_id))) {
        snprintf(s_reply_id, sizeof(s_reply_id), "%s", cmd_id);
    } else {
        s_reply_id[0] = 0;
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

/* ============================================================
 *  下行分发：+MQTTSUBRECV:0,"<topic>",<len>,<data>
 *  迁移期两套协议同时订阅着，只能靠主题区分该交给谁解析。
 * ============================================================ */
static bool urc_get_topic(const char* frame, char* out, int outsz)
{
    const char* p = strstr(frame, "+MQTTSUBRECV");
    const char* q;
    int i = 0;

    if (!p) return false;
    p = strchr(p, '"');
    if (!p) return false;
    p++;
    q = strchr(p, '"');
    if (!q) return false;

    while (i < outsz - 1 && p + i < q) {
        out[i] = p[i];
        i++;
    }
    out[i] = 0;
    return i > 0;
}

bool ESP8266_MQTT_Dispatch(const char* frame)
{
    char topic[160] = {0};

    if (frame == NULL) return false;

    if (ucTcpClosedFlag) {
        mqtt_flag = 0;
        g_onoff_state = ONOFF_IDLE;
        printf("\r\n[MQTT RX] TCP closed. MQTT link down (waiting for main loop reconnect).\r\n");
        return false;
    }

    if (!urc_get_topic(frame, topic, sizeof(topic))) {
        /* 没有可识别的主题（半截帧 / 非 SUBRECV），按旧协议兜一把再丢弃 */
        return ESP8266_MQTT_RECV(frame);
    }

#if EEG_PROTO_ENABLE
    if (strcmp(topic, EEG_TOPIC_DEV_CMD) == 0) {
        return EEG_HandleCommand(frame);
    }
#endif
#if LEGACY_PROTO_ENABLE
    if (strcmp(topic, MQTT_SUBSCRIBE_TOPIC_CTRL) == 0 ||
        strcmp(topic, MQTT_SUBSCRIBE_TOPIC_CTRLL) == 0) {
        return ESP8266_MQTT_RECV(frame);
    }
#endif

    printf("\r\n[MQTT RX] unhandled topic=%s - dropped\r\n", topic);
    return false;
}
